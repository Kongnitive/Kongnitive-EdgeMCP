/*
 * MCP OTA Update Handler — Implementation
 */

#include "mcp_ota.h"
#include <string.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "sdkconfig.h"

static const char *TAG = "mcp_ota";

/* --- OTA state --- */
static ota_state_t s_ota_state = OTA_STATE_IDLE;
static int s_ota_progress_pct = 0;
static char s_ota_message[128] = "idle";

#define OTA_BUF_SIZE 1024
#define OTA_AUTO_CONFIRM_SEC 60

/* --- Auto-confirm timer callback --- */
static void ota_auto_confirm_timer_cb(void *arg)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "Auto-confirming OTA image after %d seconds", OTA_AUTO_CONFIRM_SEC);
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
}

/* --- OTA download task --- */
static void ota_task(void *arg)
{
    char *url = (char *)arg;
    ESP_LOGI(TAG, "Starting OTA from: %s", url);

    s_ota_state = OTA_STATE_DOWNLOADING;
    s_ota_progress_pct = 0;
    snprintf(s_ota_message, sizeof(s_ota_message), "Connecting to %s", url);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        s_ota_state = OTA_STATE_ERROR;
        snprintf(s_ota_message, sizeof(s_ota_message), "HTTP client init failed");
        free(url);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        s_ota_state = OTA_STATE_ERROR;
        snprintf(s_ota_message, sizeof(s_ota_message), "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(url);
        vTaskDelete(NULL);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        /* Try to proceed anyway — chunked transfer */
        content_length = 0;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        s_ota_state = OTA_STATE_ERROR;
        snprintf(s_ota_message, sizeof(s_ota_message), "No OTA partition available");
        esp_http_client_cleanup(client);
        free(url);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Writing to partition: %s (offset 0x%lx)",
             update_partition->label, (unsigned long)update_partition->address);

    esp_ota_handle_t ota_handle;
    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        s_ota_state = OTA_STATE_ERROR;
        snprintf(s_ota_message, sizeof(s_ota_message), "OTA begin failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(url);
        vTaskDelete(NULL);
        return;
    }

    s_ota_state = OTA_STATE_WRITING;
    char *buf = malloc(OTA_BUF_SIZE);
    int total_read = 0;

    while (1) {
        int read_len = esp_http_client_read(client, buf, OTA_BUF_SIZE);
        if (read_len < 0) {
            s_ota_state = OTA_STATE_ERROR;
            snprintf(s_ota_message, sizeof(s_ota_message), "HTTP read error");
            break;
        }
        if (read_len == 0) {
            /* Done */
            break;
        }

        err = esp_ota_write(ota_handle, buf, read_len);
        if (err != ESP_OK) {
            s_ota_state = OTA_STATE_ERROR;
            snprintf(s_ota_message, sizeof(s_ota_message), "OTA write failed: %s", esp_err_to_name(err));
            break;
        }

        total_read += read_len;
        if (content_length > 0) {
            s_ota_progress_pct = (total_read * 100) / content_length;
        }
        snprintf(s_ota_message, sizeof(s_ota_message), "Written %d bytes", total_read);
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(url);

    if (s_ota_state == OTA_STATE_ERROR) {
        esp_ota_abort(ota_handle);
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        s_ota_state = OTA_STATE_ERROR;
        snprintf(s_ota_message, sizeof(s_ota_message), "OTA end failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        s_ota_state = OTA_STATE_ERROR;
        snprintf(s_ota_message, sizeof(s_ota_message), "Set boot partition failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    s_ota_state = OTA_STATE_REBOOTING;
    s_ota_progress_pct = 100;
    snprintf(s_ota_message, sizeof(s_ota_message), "OTA complete, rebooting in 2s...");
    ESP_LOGI(TAG, "OTA complete (%d bytes). Rebooting...", total_read);

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    /* never reached */
    vTaskDelete(NULL);
}

/* --- Public API --- */

esp_err_t mcp_ota_init(void)
{
    /* Check if we need to auto-confirm a pending OTA image */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "Running unconfirmed OTA image — will auto-confirm in %ds", OTA_AUTO_CONFIRM_SEC);
            const esp_timer_create_args_t timer_args = {
                .callback = ota_auto_confirm_timer_cb,
                .name = "ota_confirm",
            };
            esp_timer_handle_t timer;
            esp_timer_create(&timer_args, &timer);
            esp_timer_start_once(timer, (uint64_t)OTA_AUTO_CONFIRM_SEC * 1000000ULL);
        } else {
            ESP_LOGI(TAG, "OTA image already confirmed");
        }
    }

    ESP_LOGI(TAG, "OTA subsystem initialized (running from: %s)", running->label);
    return ESP_OK;
}

esp_err_t tool_sys_ota_push(cJSON *args, char *result, size_t max_len)
{
    if (s_ota_state == OTA_STATE_DOWNLOADING || s_ota_state == OTA_STATE_WRITING) {
        snprintf(result, max_len, "OTA already in progress (state: %d, progress: %d%%)",
                 s_ota_state, s_ota_progress_pct);
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *url_item = cJSON_GetObjectItem(args, "url");
    if (!url_item || !cJSON_IsString(url_item) || strlen(url_item->valuestring) == 0) {
        snprintf(result, max_len, "Missing or empty 'url' parameter");
        return ESP_ERR_INVALID_ARG;
    }

    /* Copy URL for the task (task will free it) */
    char *url = strdup(url_item->valuestring);
    if (!url) {
        snprintf(result, max_len, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(ota_task, "ota_task", 8192, url, 5, NULL);
    if (ret != pdPASS) {
        free(url);
        snprintf(result, max_len, "Failed to create OTA task");
        return ESP_FAIL;
    }

    snprintf(result, max_len, "OTA update started from: %s", url_item->valuestring);
    return ESP_OK;
}

esp_err_t tool_sys_ota_status(cJSON *args, char *result, size_t max_len)
{
    const char *state_str;
    switch (s_ota_state) {
        case OTA_STATE_IDLE:        state_str = "idle"; break;
        case OTA_STATE_DOWNLOADING: state_str = "downloading"; break;
        case OTA_STATE_WRITING:     state_str = "writing"; break;
        case OTA_STATE_REBOOTING:   state_str = "rebooting"; break;
        case OTA_STATE_ERROR:       state_str = "error"; break;
        default:                    state_str = "unknown"; break;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *app_desc = esp_app_get_description();

    snprintf(result, max_len,
        "{\"state\":\"%s\",\"progress_pct\":%d,\"message\":\"%s\","
        "\"partition\":\"%s\",\"app_version\":\"%s\"}",
        state_str, s_ota_progress_pct, s_ota_message,
        running ? running->label : "unknown",
        app_desc ? app_desc->version : "unknown");

    return ESP_OK;
}

esp_err_t tool_sys_ota_rollback(cJSON *args, char *result, size_t max_len)
{
    ESP_LOGW(TAG, "Rollback requested — marking app invalid and rebooting");
    snprintf(result, max_len, "Rolling back to previous firmware and rebooting...");

    /* Use a short delay so the response can be sent before reboot */
    esp_ota_mark_app_invalid_rollback_and_reboot();

    /* Should not reach here */
    return ESP_OK;
}

esp_err_t tool_sys_reboot(cJSON *args, char *result, size_t max_len)
{
    ESP_LOGW(TAG, "Reboot requested via MCP tool");
    snprintf(result, max_len, "Rebooting device...");

    /* Schedule reboot after a short delay so response can be sent */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    /* Should not reach here */
    return ESP_OK;
}
