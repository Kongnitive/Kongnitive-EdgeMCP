/*
 * MCP Protocol Handler Implementation
 */

#include "mcp_protocol.h"
#include "mcp_tools.h"
#include <string.h>
#include <esp_log.h>

static const char *TAG = "mcp_protocol";
static bool initialized = false;

esp_err_t mcp_protocol_init(void)
{
    ESP_LOGI(TAG, "Initializing MCP protocol handler");
    
    // Initialize tool registry
    esp_err_t ret = mcp_tools_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize tool registry");
        return ret;
    }
    
    initialized = false; // Will be set to true after client sends initialize
    ESP_LOGI(TAG, "MCP protocol handler ready");
    return ESP_OK;
}

bool mcp_is_initialized(void)
{
    return initialized;
}

esp_err_t mcp_handle_initialize(cJSON *params, cJSON **result)
{
    if (!params || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Handling initialize request");

    // Extract client info (optional, for logging)
    cJSON *client_info = cJSON_GetObjectItem(params, "clientInfo");
    if (client_info) {
        cJSON *name = cJSON_GetObjectItem(client_info, "name");
        cJSON *version = cJSON_GetObjectItem(client_info, "version");
        if (name && cJSON_IsString(name)) {
            ESP_LOGI(TAG, "Client: %s", name->valuestring);
        }
        if (version && cJSON_IsString(version)) {
            ESP_LOGI(TAG, "Client version: %s", version->valuestring);
        }
    }

    // Create response
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        ESP_LOGE(TAG, "Failed to create response object");
        return ESP_ERR_NO_MEM;
    }

    // Protocol version
    cJSON_AddStringToObject(response, "protocolVersion", MCP_PROTOCOL_VERSION);

    // Capabilities
    cJSON *capabilities = cJSON_CreateObject();
    cJSON *tools_cap = cJSON_CreateObject();
    cJSON_AddItemToObject(capabilities, "tools", tools_cap);
    cJSON_AddItemToObject(response, "capabilities", capabilities);

    // Server info
    cJSON *server_info = cJSON_CreateObject();
    cJSON_AddStringToObject(server_info, "name", MCP_SERVER_NAME);
    cJSON_AddStringToObject(server_info, "version", MCP_SERVER_VERSION);
    cJSON_AddItemToObject(response, "serverInfo", server_info);

    *result = response;
    initialized = true;
    
    ESP_LOGI(TAG, "MCP server initialized");
    return ESP_OK;
}

esp_err_t mcp_handle_tools_list(cJSON *params, cJSON **result)
{
    if (!result) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Handling tools/list request");

    // Get tools array from tool registry
    cJSON *tools_array = mcp_tools_get_list();
    if (!tools_array) {
        ESP_LOGE(TAG, "Failed to get tools list");
        return ESP_ERR_NO_MEM;
    }

    // Create response
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        cJSON_Delete(tools_array);
        ESP_LOGE(TAG, "Failed to create response object");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddItemToObject(response, "tools", tools_array);
    *result = response;

    ESP_LOGI(TAG, "Returned %d tools", cJSON_GetArraySize(tools_array));
    return ESP_OK;
}

esp_err_t mcp_handle_tools_call(cJSON *params, cJSON **result)
{
    if (!params || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    // Extract tool name
    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    if (!name_item || !cJSON_IsString(name_item)) {
        ESP_LOGE(TAG, "Missing or invalid 'name' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    const char *tool_name = name_item->valuestring;

    // Extract arguments
    cJSON *arguments = cJSON_GetObjectItem(params, "arguments");
    if (!arguments) {
        // Create empty arguments object if not provided
        arguments = cJSON_CreateObject();
        if (!arguments) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "Calling tool: %s", tool_name);

    // Execute tool
    char result_text[2048]; // MCP_MAX_TOOL_RESULT_SIZE
    bool is_error = false;
    esp_err_t ret = mcp_tools_execute(tool_name, arguments, result_text, sizeof(result_text), &is_error);

    // Create result object
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        ESP_LOGE(TAG, "Failed to create response object");
        return ESP_ERR_NO_MEM;
    }

    // Create content array
    cJSON *content = cJSON_CreateArray();
    cJSON *text_block = cJSON_CreateObject();
    cJSON_AddStringToObject(text_block, "type", "text");
    cJSON_AddStringToObject(text_block, "text", result_text);
    cJSON_AddItemToArray(content, text_block);
    cJSON_AddItemToObject(response, "content", content);

    // Add isError flag if tool execution failed
    if (is_error || ret != ESP_OK) {
        cJSON_AddBoolToObject(response, "isError", true);
        ESP_LOGW(TAG, "Tool execution failed: %s", result_text);
    } else {
        ESP_LOGI(TAG, "Tool executed successfully");
    }

    *result = response;
    return ESP_OK;
}

esp_err_t mcp_handle_ping(cJSON *params, cJSON **result)
{
    if (!result) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "Handling ping request");

    // Ping returns an empty object
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        ESP_LOGE(TAG, "Failed to create response object");
        return ESP_ERR_NO_MEM;
    }

    *result = response;
    return ESP_OK;
}
