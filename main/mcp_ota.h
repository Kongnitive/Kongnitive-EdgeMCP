/*
 * MCP OTA Update Handler
 *
 * Provides OTA firmware update, rollback, and status tools.
 * Uses ESP-IDF OTA APIs to download firmware from HTTP URL,
 * write to inactive partition, and reboot.
 */

#ifndef MCP_OTA_H
#define MCP_OTA_H

#include <esp_err.h>
#include <cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * OTA state machine states
 */
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_WRITING,
    OTA_STATE_REBOOTING,
    OTA_STATE_ERROR,
} ota_state_t;

/**
 * Initialize OTA subsystem.
 * Checks if running from OTA partition and starts auto-confirm timer.
 *
 * @return ESP_OK on success
 */
esp_err_t mcp_ota_init(void);

/**
 * Tool handler: sys_ota_push
 * Starts async OTA download from given URL.
 *
 * Parameters:
 *   url - HTTP(S) URL to firmware binary
 */
esp_err_t tool_sys_ota_push(cJSON *args, char *result, size_t max_len);

/**
 * Tool handler: sys_ota_status
 * Returns current OTA state and progress.
 */
esp_err_t tool_sys_ota_status(cJSON *args, char *result, size_t max_len);

/**
 * Tool handler: sys_ota_rollback
 * Marks current app invalid and reboots to previous version.
 */
esp_err_t tool_sys_ota_rollback(cJSON *args, char *result, size_t max_len);

/**
 * Tool handler: sys_reboot
 * Reboots the device.
 */
esp_err_t tool_sys_reboot(cJSON *args, char *result, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif // MCP_OTA_H
