/*
 * MCP Server Core Implementation
 */

#include "mcp_server.h"
#include "jsonrpc.h"
#include "mcp_protocol.h"
#include <string.h>
#include <stdlib.h>
#include <esp_log.h>

static const char *TAG = "mcp_server";

// Method dispatch table
typedef struct {
    const char *method;
    esp_err_t (*handler)(cJSON *params, cJSON **result);
} mcp_method_entry_t;

static const mcp_method_entry_t method_table[] = {
    {"initialize", mcp_handle_initialize},
    {"tools/list", mcp_handle_tools_list},
    {"tools/call", mcp_handle_tools_call},
    {"ping", mcp_handle_ping},
    {NULL, NULL}  // Sentinel
};

esp_err_t mcp_server_init(void)
{
    ESP_LOGI(TAG, "Initializing MCP server");
    
    esp_err_t ret = mcp_protocol_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize MCP protocol: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "MCP server initialized successfully");
    return ESP_OK;
}

static esp_err_t mcp_dispatch_method(const char *method, cJSON *params, cJSON **result)
{
    if (!method || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Find method handler
    for (const mcp_method_entry_t *entry = method_table; entry->method != NULL; entry++) {
        if (strcmp(entry->method, method) == 0) {
            return entry->handler(params, result);
        }
    }
    
    ESP_LOGW(TAG, "Method not found: %s", method);
    return ESP_ERR_NOT_FOUND;
}

char* mcp_server_process_message(const char *json_str)
{
    if (!json_str) {
        return jsonrpc_create_error(0, JSONRPC_INVALID_REQUEST, "Null message");
    }
    
    ESP_LOGD(TAG, "Processing message: %s", json_str);
    
    // Parse JSON-RPC message
    jsonrpc_message_t msg;
    esp_err_t err = jsonrpc_parse_message(json_str, &msg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse JSON-RPC message");
        return jsonrpc_create_error(0, JSONRPC_PARSE_ERROR, "Invalid JSON or JSON-RPC format");
    }
    
    char *response = NULL;
    
    // Handle request
    if (msg.type == JSONRPC_REQUEST) {
        cJSON *result = NULL;
        err = mcp_dispatch_method(msg.method, msg.params, &result);
        
        if (err == ESP_OK && result) {
            response = jsonrpc_create_response(msg.id, result);
            cJSON_Delete(result);
        } else if (err == ESP_ERR_NOT_FOUND) {
            response = jsonrpc_create_error(msg.id, JSONRPC_METHOD_NOT_FOUND, 
                                           "Method not found");
        } else if (err == ESP_ERR_INVALID_ARG) {
            response = jsonrpc_create_error(msg.id, JSONRPC_INVALID_PARAMS, 
                                           "Invalid parameters");
        } else {
            response = jsonrpc_create_error(msg.id, JSONRPC_INTERNAL_ERROR, 
                                           "Internal error");
        }
    } else if (msg.type == JSONRPC_NOTIFICATION) {
        // Notifications don't get responses
        ESP_LOGI(TAG, "Received notification: %s", msg.method);
    } else {
        response = jsonrpc_create_error(0, JSONRPC_INVALID_REQUEST, 
                                       "Invalid message type");
    }
    
    // Cleanup
    jsonrpc_message_cleanup(&msg);
    
    return response;
}

esp_err_t mcp_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "MCP client connected");
        return ESP_OK;
    }
    
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    
    // Get frame length
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGD(TAG, "Received frame len: %d", ws_pkt.len);
    
    if (ws_pkt.len) {
        // Allocate buffer for message
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for WebSocket frame");
            return ESP_ERR_NO_MEM;
        }
        
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %s", esp_err_to_name(ret));
            free(buf);
            return ret;
        }
        
        // Process message based on type
        if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
            ESP_LOGI(TAG, "Received MCP message");
            
            // Process MCP message
            char *response = mcp_server_process_message((char*)ws_pkt.payload);
            
            if (response) {
                // Send response
                httpd_ws_frame_t resp_pkt;
                memset(&resp_pkt, 0, sizeof(httpd_ws_frame_t));
                resp_pkt.type = HTTPD_WS_TYPE_TEXT;
                resp_pkt.payload = (uint8_t*)response;
                resp_pkt.len = strlen(response);
                
                ret = httpd_ws_send_frame(req, &resp_pkt);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to send response: %s", esp_err_to_name(ret));
                }
                
                free(response);
            }
        } else if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
            ESP_LOGD(TAG, "Received PING, sending PONG");
            ws_pkt.type = HTTPD_WS_TYPE_PONG;
            ret = httpd_ws_send_frame(req, &ws_pkt);
        } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
            ESP_LOGI(TAG, "Received CLOSE frame");
            ws_pkt.len = 0;
            ws_pkt.payload = NULL;
            ret = httpd_ws_send_frame(req, &ws_pkt);
        }
        
        free(buf);
    }
    
    return ret;
}

/* --- Streamable HTTP transport (POST /mcp) --- */

esp_err_t mcp_http_handler(httpd_req_t *req)
{
    /* Read POST body */
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > CONFIG_MCP_MAX_MESSAGE_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    char *body = calloc(1, content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, body + received, content_len - received);
        if (ret <= 0) {
            free(body);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Timeout");
            }
            return ESP_FAIL;
        }
        received += ret;
    }
    body[content_len] = '\0';

    ESP_LOGI(TAG, "HTTP MCP request (%d bytes)", content_len);

    /* Process through the same MCP pipeline as WebSocket */
    char *response = mcp_server_process_message(body);
    free(body);

    if (response) {
        /* Normal request -> JSON response */
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, response, strlen(response));
        free(response);
    } else {
        /* Notification -> 202 Accepted, no body */
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_send(req, NULL, 0);
    }

    return ESP_OK;
}

/* --- GET /mcp server info --- */

esp_err_t mcp_info_handler(httpd_req_t *req)
{
    const char *info =
        "{\"name\":\"" MCP_SERVER_NAME "\","
        "\"version\":\"" MCP_SERVER_VERSION "\","
        "\"protocolVersion\":\"" MCP_PROTOCOL_VERSION "\","
        "\"transports\":[\"http-post\",\"websocket\"]}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, info, strlen(info));
    return ESP_OK;
}
