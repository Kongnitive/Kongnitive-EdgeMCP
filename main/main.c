/* ESP32 MCP Server with Lua Scripting Engine and OTA Support
 *
 * Provides an MCP (Model Context Protocol) server over secure WebSocket,
 * with Lua scripting runtime, log capture, and OTA firmware update capabilities.
 */

#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include <unistd.h>
#include "esp_netif.h"
#include "esp_wifi.h"
#include "wifi_manager.h"
#include <esp_https_server.h>
#include <esp_http_server.h>
#include "keep_alive.h"
#include "mcp_server.h"
#include "mcp_log.h"
#include "mcp_ota.h"
#include "lua_runtime.h"
#include "sdkconfig.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if !CONFIG_HTTPD_WS_SUPPORT
#error This requires HTTPD_WS_SUPPORT enabled in esp-http-server component configuration
#endif

static const char *TAG = "mcp_main";
static const size_t max_clients = 4;

struct async_resp_arg {
    httpd_handle_t hd;
    int fd;
};

/* --- WebSocket connection management --- */

esp_err_t wss_open_fd(httpd_handle_t hd, int sockfd)
{
    ESP_LOGI(TAG, "New client connected %d", sockfd);
    wss_keep_alive_t h = httpd_get_global_user_ctx(hd);
    return wss_keep_alive_add_client(h, sockfd);
}

void wss_close_fd(httpd_handle_t hd, int sockfd)
{
    ESP_LOGI(TAG, "Client disconnected %d", sockfd);
    wss_keep_alive_t h = httpd_get_global_user_ctx(hd);
    wss_keep_alive_remove_client(h, sockfd);
    close(sockfd);
}

/* MCP WebSocket endpoint (legacy / direct WS clients) */
static const httpd_uri_t mcp_ws = {
    .uri        = "/mcp",
    .method     = HTTP_GET,
    .handler    = mcp_ws_handler,
    .user_ctx   = NULL,
    .is_websocket = true,
    .handle_ws_control_frames = true
};

/* MCP HTTP POST endpoint (streamable-http transport for Claude Code) */
static const httpd_uri_t mcp_http = {
    .uri        = "/mcp",
    .method     = HTTP_POST,
    .handler    = mcp_http_handler,
    .user_ctx   = NULL,
};

/* MCP GET /mcp info endpoint */
static const httpd_uri_t mcp_info = {
    .uri        = "/mcp",
    .method     = HTTP_GET,
    .handler    = mcp_info_handler,
    .user_ctx   = NULL,
};

static void send_ping(void *arg)
{
    struct async_resp_arg *resp_arg = arg;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_PING;
    httpd_ws_send_frame_async(resp_arg->hd, resp_arg->fd, &ws_pkt);
    free(resp_arg);
}

bool client_not_alive_cb(wss_keep_alive_t h, int fd)
{
    ESP_LOGE(TAG, "Client not alive, closing fd %d", fd);
    httpd_sess_trigger_close(wss_keep_alive_get_user_ctx(h), fd);
    return true;
}

bool check_client_alive_cb(wss_keep_alive_t h, int fd)
{
    ESP_LOGD(TAG, "Checking if client (fd=%d) is alive", fd);
    struct async_resp_arg *resp_arg = malloc(sizeof(struct async_resp_arg));
    if (!resp_arg) return false;
    resp_arg->hd = wss_keep_alive_get_user_ctx(h);
    resp_arg->fd = fd;
    return httpd_queue_work(resp_arg->hd, send_ping, resp_arg) == ESP_OK;
}

/* --- Plain HTTP server (no TLS, for easier MCP client testing) --- */

static httpd_handle_t start_http_server(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = max_clients;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.lru_purge_enable = true;
    config.stack_size = 8192;                   /* larger stack for WiFi API calls */

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error starting HTTP server on port 80");
        return NULL;
    }

    httpd_register_uri_handler(server, &mcp_http);
    httpd_register_uri_handler(server, &mcp_info);
    ESP_LOGI(TAG, "HTTP server started, MCP at http://<ip>/mcp (POST)");
    return server;
}

/* --- HTTPS/WSS server --- */

static httpd_handle_t start_mcp_server(void)
{
    wss_keep_alive_config_t keep_alive_config = KEEP_ALIVE_CONFIG_DEFAULT();
    keep_alive_config.max_clients = max_clients;
    keep_alive_config.client_not_alive_cb = client_not_alive_cb;
    keep_alive_config.check_client_alive_cb = check_client_alive_cb;
    wss_keep_alive_t keep_alive = wss_keep_alive_start(&keep_alive_config);

    httpd_handle_t server = NULL;
    ESP_LOGI(TAG, "Starting HTTPS server");

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.httpd.max_open_sockets = max_clients;
    conf.httpd.stack_size = 8192;
    conf.httpd.global_user_ctx = keep_alive;
    conf.httpd.open_fn = wss_open_fd;
    conf.httpd.close_fn = wss_close_fd;

    extern const unsigned char servercert_start[] asm("_binary_servercert_pem_start");
    extern const unsigned char servercert_end[]   asm("_binary_servercert_pem_end");
    conf.servercert = servercert_start;
    conf.servercert_len = servercert_end - servercert_start;

    extern const unsigned char prvtkey_pem_start[] asm("_binary_prvtkey_pem_start");
    extern const unsigned char prvtkey_pem_end[]   asm("_binary_prvtkey_pem_end");
    conf.prvtkey_pem = prvtkey_pem_start;
    conf.prvtkey_len = prvtkey_pem_end - prvtkey_pem_start;

    esp_err_t ret = httpd_ssl_start(&server, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error starting server!");
        return NULL;
    }

    /* Register both transports on /mcp */
    ESP_LOGI(TAG, "Registering MCP endpoints at /mcp (WSS + HTTP POST)");
    httpd_register_uri_handler(server, &mcp_ws);
    httpd_register_uri_handler(server, &mcp_http);
    wss_keep_alive_set_user_ctx(keep_alive, server);

    /* Initialize MCP server */
    esp_err_t mcp_ret = mcp_server_init();
    if (mcp_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize MCP server: %s", esp_err_to_name(mcp_ret));
    } else {
        ESP_LOGI(TAG, "MCP server initialized, available at wss://<ip>/mcp");
    }

    return server;
}

static esp_err_t stop_mcp_server(httpd_handle_t server)
{
    wss_keep_alive_stop(httpd_get_global_user_ctx(server));
    return httpd_ssl_stop(server);
}

/* --- WiFi event handlers --- */

static void disconnect_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server) {
        if (stop_mcp_server(*server) == ESP_OK) {
            *server = NULL;
        } else {
            ESP_LOGE(TAG, "Failed to stop server");
        }
    }
}

static void connect_handler(void *arg, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server == NULL) {
        *server = start_mcp_server();
        start_http_server();
    }
}

/* --- Application entry point --- */

void app_main(void)
{
    static httpd_handle_t server = NULL;

    /* Initialize log capture first, before anything else logs */
    mcp_log_init();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Connect to WiFi (non-blocking: continue even if WiFi fails) */
    ESP_LOGI(TAG, "Connecting to WiFi...");
    esp_err_t wifi_result = wifi_manager_connect();
    if (wifi_result != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connection failed, continuing without network");
    }

    /* Register WiFi reconnection handlers */
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));

    /* Initialize OTA subsystem (auto-confirm timer if needed) */
    mcp_ota_init();

    /* Start servers only if WiFi is connected; otherwise connect_handler will start them later */
    if (wifi_result == ESP_OK) {
        server = start_mcp_server();
        start_http_server();
    }

    /* Initialize and start Lua scripting runtime */
    esp_err_t lua_ret = lua_runtime_init();
    if (lua_ret == ESP_OK) {
        lua_runtime_start();
        ESP_LOGI(TAG, "Lua runtime started, executing main.lua");
    } else {
        ESP_LOGE(TAG, "Failed to initialize Lua runtime: %s", esp_err_to_name(lua_ret));
    }

    ESP_LOGI(TAG, "System ready. MCP at https://<ip>/mcp (POST) or wss://<ip>/mcp (WS)");
}
