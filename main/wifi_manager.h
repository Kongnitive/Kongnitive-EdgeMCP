/*
 * WiFi Manager
 *
 * Simple WiFi connection management for MCP server
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize and connect to WiFi
 *
 * WiFi credentials are configured via menuconfig:
 * - CONFIG_MCP_WIFI_SSID
 * - CONFIG_MCP_WIFI_PASSWORD
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_connect(void);

/**
 * Disconnect from WiFi and cleanup
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_disconnect(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
