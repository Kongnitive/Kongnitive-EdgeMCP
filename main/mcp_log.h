/*
 * MCP Log Capture System
 *
 * Ring buffer log capture with ESP-IDF log hook.
 * Provides sys_get_logs tool for retrieving recent log lines.
 */

#ifndef MCP_LOG_H
#define MCP_LOG_H

#include <esp_err.h>
#include <cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the log capture system.
 * Installs a custom vprintf hook to capture ESP_LOGx output.
 * Must be called early in app_main, before other components log.
 *
 * @return ESP_OK on success
 */
esp_err_t mcp_log_init(void);

/**
 * Tool handler: sys_get_logs
 * Returns filtered log lines from the ring buffer.
 *
 * Parameters (via cJSON args):
 *   level  - minimum log level: "error","warn","info","debug","verbose" (default "info")
 *   lines  - max number of lines to return (default 20)
 *   filter - substring match filter (optional)
 */
esp_err_t tool_sys_get_logs(cJSON *args, char *result, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif // MCP_LOG_H
