/*
 * MCP Protocol Handler
 * 
 * Implements Model Context Protocol methods
 */

#ifndef MCP_PROTOCOL_H
#define MCP_PROTOCOL_H

#include <stdbool.h>
#include <esp_err.h>
#include <cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * MCP protocol version
 */
#define MCP_PROTOCOL_VERSION "2024-11-05"

/**
 * Server information
 */
#define MCP_SERVER_NAME "esp32-mcp-server"
#define MCP_SERVER_VERSION "1.0.0"

/**
 * MCP-specific error codes (extending JSON-RPC)
 */
typedef enum {
    MCP_ERROR_TOOL_NOT_FOUND = -32001,      // Tool not found
    MCP_ERROR_TOOL_EXECUTION = -32002,      // Tool execution failed
    MCP_ERROR_NOT_INITIALIZED = -32003      // Server not initialized
} mcp_error_code_t;

/**
 * Initialize the MCP protocol handler
 * Must be called before any other MCP functions
 * 
 * @return ESP_OK on success
 */
esp_err_t mcp_protocol_init(void);

/**
 * Handle MCP initialize method
 * 
 * @param params Request parameters
 * @param result Output result object (caller must free with cJSON_Delete)
 * @return ESP_OK on success
 */
esp_err_t mcp_handle_initialize(cJSON *params, cJSON **result);

/**
 * Handle MCP tools/list method
 * 
 * @param params Request parameters (unused)
 * @param result Output result object (caller must free with cJSON_Delete)
 * @return ESP_OK on success
 */
esp_err_t mcp_handle_tools_list(cJSON *params, cJSON **result);

/**
 * Handle MCP tools/call method
 * 
 * @param params Request parameters (must contain "name" and "arguments")
 * @param result Output result object (caller must free with cJSON_Delete)
 * @return ESP_OK on success
 */
esp_err_t mcp_handle_tools_call(cJSON *params, cJSON **result);

/**
 * Handle MCP ping method
 * 
 * @param params Request parameters (unused)
 * @param result Output result object (caller must free with cJSON_Delete)
 * @return ESP_OK on success
 */
esp_err_t mcp_handle_ping(cJSON *params, cJSON **result);

/**
 * Check if MCP server is initialized
 * 
 * @return true if initialized
 */
bool mcp_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif // MCP_PROTOCOL_H
