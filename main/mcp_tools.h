/*
 * MCP Tool Registry and Dispatcher
 *
 * Manages tool registration and execution
 */

#ifndef MCP_TOOLS_H
#define MCP_TOOLS_H

#include <esp_err.h>
#include <cJSON.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Maximum tool result size
 */
#define MCP_MAX_TOOL_RESULT_SIZE 2048

/**
 * Tool handler function type
 *
 * @param arguments Tool arguments (cJSON object)
 * @param result_text Output buffer for result text
 * @param max_len Maximum length of result_text buffer
 * @return ESP_OK on success, error code on failure
 */
typedef esp_err_t (*mcp_tool_handler_t)(cJSON *arguments, char *result_text, size_t max_len);

/**
 * Tool definition structure
 */
typedef struct {
    const char *name;                   // Tool name
    const char *description;            // Tool description
    const char *input_schema_json;      // Pre-serialized JSON schema
    mcp_tool_handler_t handler;         // Tool handler function
} mcp_tool_t;

/**
 * Initialize the tool registry
 *
 * @return ESP_OK on success
 */
esp_err_t mcp_tools_init(void);

/**
 * Get list of all registered tools
 * Returns a cJSON array of tool definitions
 *
 * @return cJSON array (caller must free with cJSON_Delete), or NULL on error
 */
cJSON* mcp_tools_get_list(void);

/**
 * Execute a tool by name
 *
 * @param tool_name Name of the tool to execute
 * @param arguments Tool arguments (cJSON object)
 * @param result_text Output buffer for result text
 * @param max_len Maximum length of result_text buffer
 * @param is_error Output flag indicating if execution resulted in error
 * @return ESP_OK on success, error code on failure
 */
esp_err_t mcp_tools_execute(const char *tool_name, cJSON *arguments,
                            char *result_text, size_t max_len, bool *is_error);

/**
 * Find a tool by name
 *
 * @param name Tool name
 * @return Pointer to tool definition, or NULL if not found
 */
const mcp_tool_t* mcp_tools_find(const char *name);

#ifdef __cplusplus
}
#endif

#endif // MCP_TOOLS_H
