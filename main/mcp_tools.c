/*
 * MCP Tool Registry and Dispatcher Implementation
 */

#include "mcp_tools.h"
#include "mcp_log.h"
#include "mcp_ota.h"
#include "lua_runtime.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <driver/gpio.h>

static const char *TAG = "mcp_tools";

static const char *PROJECT_SYSTEM_PROMPT =
    "You are controlling an ESP32 MCP server with a Lua runtime.\n"
    "Goal: modify device behavior by editing Lua scripts in /spiffs, not by changing firmware unless required.\n"
    "Core loop: sys_get_logs -> lua_get_script -> edit -> lua_push_script -> lua_restart -> verify logs.\n"
    "For DI display switching, prefer lua_bind_dependency to update bindings.lua.\n"
    "Default display interface is 'display' with providers like 'mock_display'.\n"
    "Useful tools: get_status, sys_get_logs, lua_list_scripts, lua_get_script, lua_push_script, lua_bind_dependency, lua_restart, lua_exec.\n"
    "Safety: keep script changes small, verify each step, and rollback by restoring previous script content if needed.";

// Forward declarations of tool handlers
static esp_err_t tool_control_led(cJSON *args, char *result, size_t max_len);
static esp_err_t tool_get_status(cJSON *args, char *result, size_t max_len);
static esp_err_t tool_get_system_prompt(cJSON *args, char *result, size_t max_len);
static esp_err_t tool_lua_push_script(cJSON *args, char *result, size_t max_len);
static esp_err_t tool_lua_get_script(cJSON *args, char *result, size_t max_len);
static esp_err_t tool_lua_list_scripts(cJSON *args, char *result, size_t max_len);
static esp_err_t tool_lua_exec(cJSON *args, char *result, size_t max_len);
static esp_err_t tool_lua_restart(cJSON *args, char *result, size_t max_len);
static esp_err_t tool_lua_bind_dependency(cJSON *args, char *result, size_t max_len);

// Tool registry (static, compile-time)
static const mcp_tool_t tool_registry[] = {
    {
        .name = "control_led",
        .description = "Control the onboard LED",
        .input_schema_json = 
            "{\"type\":\"object\","
            "\"properties\":{\"state\":{\"type\":\"string\",\"enum\":[\"on\",\"off\",\"toggle\"],\"description\":\"LED state\"}},"
            "\"required\":[\"state\"]}",
        .handler = tool_control_led
    },
    {
        .name = "get_status",
        .description = "Get system status information including memory, WiFi, and uptime",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .handler = tool_get_status
    },
    {
        .name = "get_system_prompt",
        .description = "Get the overall project prompt for AI agents (what this project does and recommended tool workflow)",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .handler = tool_get_system_prompt
    },
    {
        .name = "sys_get_logs",
        .description = "Retrieve recent runtime logs from the device",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"level\":{\"type\":\"string\",\"enum\":[\"error\",\"warn\",\"info\",\"debug\",\"verbose\"],\"description\":\"Minimum log level filter\",\"default\":\"info\"},"
            "\"lines\":{\"type\":\"integer\",\"description\":\"Max number of log lines to return\",\"default\":20},"
            "\"filter\":{\"type\":\"string\",\"description\":\"Substring filter for log messages\"}"
            "}}",
        .handler = tool_sys_get_logs
    },
    {
        .name = "sys_ota_push",
        .description = "Start OTA firmware update from HTTP URL",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"url\":{\"type\":\"string\",\"description\":\"HTTP URL to firmware binary\"}"
            "},"
            "\"required\":[\"url\"]}",
        .handler = tool_sys_ota_push
    },
    {
        .name = "sys_ota_status",
        .description = "Get current OTA update state and progress",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .handler = tool_sys_ota_status
    },
    {
        .name = "sys_ota_rollback",
        .description = "Rollback to previous firmware version and reboot",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .handler = tool_sys_ota_rollback
    },
    {
        .name = "sys_reboot",
        .description = "Reboot the device",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .handler = tool_sys_reboot
    },
    {
        .name = "lua_push_script",
        .description = "Write or update a Lua script on the device. Use append=true for large scripts sent in chunks.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Script filename (e.g. main.lua)\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"Lua source code\"},"
            "\"append\":{\"type\":\"boolean\",\"description\":\"Append to existing file instead of overwrite\",\"default\":false}"
            "},"
            "\"required\":[\"name\",\"content\"]}",
        .handler = tool_lua_push_script
    },
    {
        .name = "lua_get_script",
        .description = "Read a Lua script's source code from the device",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Script filename (e.g. main.lua)\"}"
            "},"
            "\"required\":[\"name\"]}",
        .handler = tool_lua_get_script
    },
    {
        .name = "lua_list_scripts",
        .description = "List all Lua scripts stored on the device",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .handler = tool_lua_list_scripts
    },
    {
        .name = "lua_exec",
        .description = "Execute a Lua code snippet directly in the VM and return the result",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"code\":{\"type\":\"string\",\"description\":\"Lua code to execute\"}"
            "},"
            "\"required\":[\"code\"]}",
        .handler = tool_lua_exec
    },
    {
        .name = "lua_bind_dependency",
        .description = "Bind a DI interface to a provider by updating bindings.lua and optionally restart Lua VM",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"provider\":{\"type\":\"string\",\"description\":\"Provider name (e.g. ssd1306 or mock_display)\"},"
            "\"interface\":{\"type\":\"string\",\"description\":\"Interface name, default is display\",\"default\":\"display\"},"
            "\"opts\":{\"type\":\"object\",\"description\":\"Provider options table written into bindings.lua\"},"
            "\"restart\":{\"type\":\"boolean\",\"description\":\"Restart Lua VM after updating bindings\",\"default\":true}"
            "},"
            "\"required\":[\"provider\"]}",
        .handler = tool_lua_bind_dependency
    },
    {
        .name = "lua_restart",
        .description = "Restart the Lua VM, re-executing main.lua with any recent script changes",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .handler = tool_lua_restart
    },
    {NULL, NULL, NULL, NULL}  // Sentinel
};

// LED GPIO configuration
#define LED_GPIO CONFIG_BLINK_GPIO
static bool led_initialized = false;

esp_err_t mcp_tools_init(void)
{
    ESP_LOGI(TAG, "Initializing tool registry");
    
    // Initialize LED GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret == ESP_OK) {
        led_initialized = true;
        gpio_set_level(LED_GPIO, 0); // Start with LED off
        ESP_LOGI(TAG, "LED GPIO initialized on pin %d", LED_GPIO);
    } else {
        ESP_LOGW(TAG, "Failed to initialize LED GPIO: %s", esp_err_to_name(ret));
    }
    
    // Count registered tools
    int tool_count = 0;
    for (const mcp_tool_t *tool = tool_registry; tool->name != NULL; tool++) {
        tool_count++;
    }
    
    ESP_LOGI(TAG, "Tool registry initialized with %d tools", tool_count);
    return ESP_OK;
}

const mcp_tool_t* mcp_tools_find(const char *name)
{
    if (!name) {
        return NULL;
    }
    
    for (const mcp_tool_t *tool = tool_registry; tool->name != NULL; tool++) {
        if (strcmp(tool->name, name) == 0) {
            return tool;
        }
    }
    
    return NULL;
}

cJSON* mcp_tools_get_list(void)
{
    cJSON *tools_array = cJSON_CreateArray();
    if (!tools_array) {
        ESP_LOGE(TAG, "Failed to create tools array");
        return NULL;
    }
    
    for (const mcp_tool_t *tool = tool_registry; tool->name != NULL; tool++) {
        cJSON *tool_obj = cJSON_CreateObject();
        if (!tool_obj) {
            ESP_LOGE(TAG, "Failed to create tool object");
            cJSON_Delete(tools_array);
            return NULL;
        }
        
        cJSON_AddStringToObject(tool_obj, "name", tool->name);
        cJSON_AddStringToObject(tool_obj, "description", tool->description);
        
        // Parse and add input schema
        cJSON *schema = cJSON_Parse(tool->input_schema_json);
        if (schema) {
            cJSON_AddItemToObject(tool_obj, "inputSchema", schema);
        } else {
            ESP_LOGW(TAG, "Failed to parse schema for tool: %s", tool->name);
            // Add empty schema as fallback
            cJSON_AddItemToObject(tool_obj, "inputSchema", cJSON_CreateObject());
        }
        
        cJSON_AddItemToArray(tools_array, tool_obj);
    }
    
    return tools_array;
}

esp_err_t mcp_tools_execute(const char *tool_name, cJSON *arguments,
                            char *result_text, size_t max_len, bool *is_error)
{
    if (!tool_name || !result_text || !is_error) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *is_error = false;
    
    // Find tool
    const mcp_tool_t *tool = mcp_tools_find(tool_name);
    if (!tool) {
        snprintf(result_text, max_len, "Tool not found: %s", tool_name);
        *is_error = true;
        return ESP_ERR_NOT_FOUND;
    }
    
    // Execute tool handler
    esp_err_t ret = tool->handler(arguments, result_text, max_len);
    if (ret != ESP_OK) {
        *is_error = true;
        // If handler didn't set error message, set a generic one
        if (strlen(result_text) == 0) {
            snprintf(result_text, max_len, "Tool execution failed: %s", esp_err_to_name(ret));
        }
    }
    
    return ret;
}

// ============================================================================
// Tool Implementations
// ============================================================================

static esp_err_t tool_control_led(cJSON *args, char *result, size_t max_len)
{
    if (!led_initialized) {
        snprintf(result, max_len, "LED not initialized (GPIO %d not available)", LED_GPIO);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Extract state parameter
    cJSON *state_item = cJSON_GetObjectItem(args, "state");
    if (!state_item || !cJSON_IsString(state_item)) {
        snprintf(result, max_len, "Missing or invalid 'state' parameter. Must be 'on', 'off', or 'toggle'");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *state = state_item->valuestring;
    
    // Execute command
    if (strcmp(state, "on") == 0) {
        gpio_set_level(LED_GPIO, 1);
        snprintf(result, max_len, "LED turned on (GPIO %d)", LED_GPIO);
    } else if (strcmp(state, "off") == 0) {
        gpio_set_level(LED_GPIO, 0);
        snprintf(result, max_len, "LED turned off (GPIO %d)", LED_GPIO);
    } else if (strcmp(state, "toggle") == 0) {
        int current = gpio_get_level(LED_GPIO);
        gpio_set_level(LED_GPIO, !current);
        snprintf(result, max_len, "LED toggled to %s (GPIO %d)", !current ? "on" : "off", LED_GPIO);
    } else {
        snprintf(result, max_len, "Invalid state: '%s'. Must be 'on', 'off', or 'toggle'", state);
        return ESP_ERR_INVALID_ARG;
    }
    
    return ESP_OK;
}

static esp_err_t tool_get_status(cJSON *args, char *result, size_t max_len)
{
    (void)args;

    // Get system information
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_free_heap = esp_get_minimum_free_heap_size();
    uint64_t uptime_sec = esp_timer_get_time() / 1000000ULL;
    
    // Get WiFi info
    wifi_ap_record_t ap_info;
    memset(&ap_info, 0, sizeof(ap_info));
    esp_err_t wifi_ret = esp_wifi_sta_get_ap_info(&ap_info);
    int rssi = (wifi_ret == ESP_OK) ? ap_info.rssi : 0;
    
    // Get LED state
    int led_state = led_initialized ? gpio_get_level(LED_GPIO) : -1;
    
    // Format result
    int written = snprintf(result, max_len,
        "ESP32 System Status:\n"
        "-------------------\n"
        "Free Heap: %lu bytes (%.1f KB)\n"
        "Min Free Heap: %lu bytes (%.1f KB)\n"
        "Uptime: %llu seconds (%.1f hours)\n",
        free_heap, free_heap / 1024.0,
        min_free_heap, min_free_heap / 1024.0,
        uptime_sec, uptime_sec / 3600.0);
    
    if (wifi_ret == ESP_OK) {
        written += snprintf(result + written, max_len - written,
            "WiFi SSID: %s\n"
            "WiFi RSSI: %d dBm\n",
            ap_info.ssid, rssi);
    } else {
        written += snprintf(result + written, max_len - written,
            "WiFi: Not connected\n");
    }
    
    if (led_initialized) {
        written += snprintf(result + written, max_len - written,
            "LED State: %s (GPIO %d)\n",
            led_state ? "ON" : "OFF", LED_GPIO);
    } else {
        written += snprintf(result + written, max_len - written,
            "LED: Not initialized\n");
    }

    snprintf(result + written, max_len - written,
        "Project Prompt: call get_system_prompt for agent workflow and usage guidance");

    return ESP_OK;
}

static esp_err_t tool_get_system_prompt(cJSON *args, char *result, size_t max_len)
{
    (void)args;
    snprintf(result, max_len, "%s", PROJECT_SYSTEM_PROMPT);
    return ESP_OK;
}

// ============================================================================
// Lua Tool Implementations
// ============================================================================

static bool strbuf_append(char **cursor, size_t *remaining, const char *text)
{
    int n = snprintf(*cursor, *remaining, "%s", text);
    if (n < 0 || (size_t)n >= *remaining) {
        return false;
    }
    *cursor += n;
    *remaining -= (size_t)n;
    return true;
}

static bool strbuf_appendf(char **cursor, size_t *remaining, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(*cursor, *remaining, fmt, args);
    va_end(args);

    if (n < 0 || (size_t)n >= *remaining) {
        return false;
    }

    *cursor += n;
    *remaining -= (size_t)n;
    return true;
}

static bool strbuf_append_lua_string(char **cursor, size_t *remaining, const char *value)
{
    if (!strbuf_append(cursor, remaining, "\"")) {
        return false;
    }

    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        switch (*p) {
            case '\\':
                if (!strbuf_append(cursor, remaining, "\\\\")) return false;
                break;
            case '"':
                if (!strbuf_append(cursor, remaining, "\\\"")) return false;
                break;
            case '\n':
                if (!strbuf_append(cursor, remaining, "\\n")) return false;
                break;
            case '\r':
                if (!strbuf_append(cursor, remaining, "\\r")) return false;
                break;
            case '\t':
                if (!strbuf_append(cursor, remaining, "\\t")) return false;
                break;
            default:
                if (*p < 0x20) {
                    if (!strbuf_appendf(cursor, remaining, "\\x%02X", *p)) return false;
                } else {
                    if (!strbuf_appendf(cursor, remaining, "%c", *p)) return false;
                }
                break;
        }
    }

    return strbuf_append(cursor, remaining, "\"");
}

static bool serialize_cjson_to_lua(cJSON *value, char **cursor, size_t *remaining)
{
    if (cJSON_IsNull(value)) {
        return strbuf_append(cursor, remaining, "nil");
    }

    if (cJSON_IsTrue(value) || cJSON_IsFalse(value)) {
        return strbuf_append(cursor, remaining, cJSON_IsTrue(value) ? "true" : "false");
    }

    if (cJSON_IsNumber(value)) {
        if ((double)value->valueint == value->valuedouble) {
            return strbuf_appendf(cursor, remaining, "%d", value->valueint);
        }
        return strbuf_appendf(cursor, remaining, "%.17g", value->valuedouble);
    }

    if (cJSON_IsString(value) && value->valuestring) {
        return strbuf_append_lua_string(cursor, remaining, value->valuestring);
    }

    if (cJSON_IsArray(value)) {
        if (!strbuf_append(cursor, remaining, "{")) {
            return false;
        }

        bool first = true;
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, value) {
            if (!first && !strbuf_append(cursor, remaining, ", ")) {
                return false;
            }
            if (!serialize_cjson_to_lua(item, cursor, remaining)) {
                return false;
            }
            first = false;
        }

        return strbuf_append(cursor, remaining, "}");
    }

    if (cJSON_IsObject(value)) {
        if (!strbuf_append(cursor, remaining, "{")) {
            return false;
        }

        bool first = true;
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, value) {
            if (!first && !strbuf_append(cursor, remaining, ", ")) {
                return false;
            }
            if (!strbuf_append(cursor, remaining, "[")) {
                return false;
            }
            if (!strbuf_append_lua_string(cursor, remaining, item->string ? item->string : "")) {
                return false;
            }
            if (!strbuf_append(cursor, remaining, "] = ")) {
                return false;
            }
            if (!serialize_cjson_to_lua(item, cursor, remaining)) {
                return false;
            }
            first = false;
        }

        return strbuf_append(cursor, remaining, "}");
    }

    return false;
}

static bool build_bindings_lua_script(const char *interface_name, const char *provider,
                                      cJSON *opts, char *out, size_t out_len)
{
    char *cursor = out;
    size_t remaining = out_len;

    if (!strbuf_append(&cursor, &remaining, "return {\n    [")) return false;
    if (!strbuf_append_lua_string(&cursor, &remaining, interface_name)) return false;
    if (!strbuf_append(&cursor, &remaining, "] = {\n        provider = ")) return false;
    if (!strbuf_append_lua_string(&cursor, &remaining, provider)) return false;
    if (!strbuf_append(&cursor, &remaining, ",\n        opts = ")) return false;

    if (opts) {
        if (!serialize_cjson_to_lua(opts, &cursor, &remaining)) return false;
    } else {
        if (!strbuf_append(&cursor, &remaining, "{}")) return false;
    }

    if (!strbuf_append(&cursor, &remaining, "\n    }\n}\n")) return false;
    return true;
}

static esp_err_t tool_lua_bind_dependency(cJSON *args, char *result, size_t max_len)
{
    if (!args || !cJSON_IsObject(args)) {
        snprintf(result, max_len, "Missing arguments object");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *provider_item = cJSON_GetObjectItem(args, "provider");
    if (!provider_item || !cJSON_IsString(provider_item) || !provider_item->valuestring ||
        provider_item->valuestring[0] == '\0') {
        snprintf(result, max_len, "Missing required parameter: provider");
        return ESP_ERR_INVALID_ARG;
    }

    const char *interface_name = "display";
    cJSON *interface_item = cJSON_GetObjectItem(args, "interface");
    if (interface_item) {
        if (!cJSON_IsString(interface_item) || !interface_item->valuestring ||
            interface_item->valuestring[0] == '\0') {
            snprintf(result, max_len, "Invalid parameter: interface must be non-empty string");
            return ESP_ERR_INVALID_ARG;
        }
        interface_name = interface_item->valuestring;
    }

    cJSON *opts_item = cJSON_GetObjectItem(args, "opts");
    if (opts_item && !cJSON_IsObject(opts_item)) {
        snprintf(result, max_len, "Invalid parameter: opts must be object");
        return ESP_ERR_INVALID_ARG;
    }

    bool restart = true;
    cJSON *restart_item = cJSON_GetObjectItem(args, "restart");
    if (restart_item) {
        if (!cJSON_IsTrue(restart_item) && !cJSON_IsFalse(restart_item)) {
            snprintf(result, max_len, "Invalid parameter: restart must be boolean");
            return ESP_ERR_INVALID_ARG;
        }
        restart = cJSON_IsTrue(restart_item);
    }

    char bindings_script[2048];
    if (!build_bindings_lua_script(interface_name, provider_item->valuestring,
                                   opts_item, bindings_script, sizeof(bindings_script))) {
        snprintf(result, max_len, "Failed to generate bindings.lua (payload too large or unsupported type)");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = lua_runtime_push_script("bindings.lua", bindings_script, false);
    if (ret != ESP_OK) {
        snprintf(result, max_len, "Failed to write bindings.lua");
        return ret;
    }

    if (restart) {
        ret = lua_runtime_restart();
        if (ret != ESP_OK) {
            snprintf(result, max_len,
                     "bindings.lua updated: %s -> %s, but lua_restart failed",
                     interface_name, provider_item->valuestring);
            return ret;
        }
    }

    snprintf(result, max_len, "Binding updated: %s -> %s (restart=%s)",
             interface_name, provider_item->valuestring, restart ? "true" : "false");
    return ESP_OK;
}

static esp_err_t tool_lua_push_script(cJSON *args, char *result, size_t max_len)
{
    cJSON *name_item = cJSON_GetObjectItem(args, "name");
    cJSON *content_item = cJSON_GetObjectItem(args, "content");
    if (!name_item || !cJSON_IsString(name_item) ||
        !content_item || !cJSON_IsString(content_item)) {
        snprintf(result, max_len, "Missing required parameters: name, content");
        return ESP_ERR_INVALID_ARG;
    }

    bool append = false;
    cJSON *append_item = cJSON_GetObjectItem(args, "append");
    if (append_item && cJSON_IsTrue(append_item)) {
        append = true;
    }

    esp_err_t ret = lua_runtime_push_script(name_item->valuestring,
                                             content_item->valuestring, append);
    if (ret == ESP_OK) {
        snprintf(result, max_len, "Script '%s' %s (%d bytes)",
                 name_item->valuestring,
                 append ? "appended" : "written",
                 (int)strlen(content_item->valuestring));
    } else {
        snprintf(result, max_len, "Failed to write script '%s'", name_item->valuestring);
    }
    return ret;
}

static esp_err_t tool_lua_get_script(cJSON *args, char *result, size_t max_len)
{
    cJSON *name_item = cJSON_GetObjectItem(args, "name");
    if (!name_item || !cJSON_IsString(name_item)) {
        snprintf(result, max_len, "Missing required parameter: name");
        return ESP_ERR_INVALID_ARG;
    }

    return lua_runtime_get_script(name_item->valuestring, result, max_len);
}

static esp_err_t tool_lua_list_scripts(cJSON *args, char *result, size_t max_len)
{
    (void)args;
    return lua_runtime_list_scripts(result, max_len);
}

static esp_err_t tool_lua_exec(cJSON *args, char *result, size_t max_len)
{
    cJSON *code_item = cJSON_GetObjectItem(args, "code");
    if (!code_item || !cJSON_IsString(code_item)) {
        snprintf(result, max_len, "Missing required parameter: code");
        return ESP_ERR_INVALID_ARG;
    }

    return lua_runtime_exec(code_item->valuestring, result, max_len);
}

static esp_err_t tool_lua_restart(cJSON *args, char *result, size_t max_len)
{
    (void)args;
    esp_err_t ret = lua_runtime_restart();
    if (ret == ESP_OK) {
        snprintf(result, max_len, "Lua VM restarted, main.lua re-executing");
    } else {
        snprintf(result, max_len, "Failed to restart Lua VM");
    }
    return ret;
}
