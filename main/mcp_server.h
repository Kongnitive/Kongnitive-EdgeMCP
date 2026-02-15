/*
 * MCP Server Core
 *
 * Main entry point for MCP server functionality
 */

#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <esp_err.h>
#include <esp_http_server.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the MCP server
 * Must be called before processing any MCP messages
 *
 * @return ESP_OK on success
 */
esp_err_t mcp_server_init(void);

/**
 * Process an incoming MCP message
 *
 * @param json_str Input JSON-RPC message
 * @return Response JSON string (caller must free), or NULL on error
 */
char* mcp_server_process_message(const char *json_str);

/**
 * WebSocket handler for MCP
 */
esp_err_t mcp_ws_handler(httpd_req_t *req);

/**
 * HTTP POST handler for MCP (streamable-http transport)
 */
esp_err_t mcp_http_handler(httpd_req_t *req);

/**
 * GET /mcp info handler - returns server info as JSON
 */
esp_err_t mcp_info_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif // MCP_SERVER_H
