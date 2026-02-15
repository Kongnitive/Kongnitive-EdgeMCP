/*
 * Lua Runtime for ESP32
 *
 * Manages Lua VM lifecycle, SPIFFS script storage, and C bindings
 * for hardware access (GPIO, timers, logging, system info).
 */

#ifndef LUA_RUNTIME_H
#define LUA_RUNTIME_H

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize SPIFFS and Lua VM, register C bindings.
 * If no main.lua exists on SPIFFS, writes the default script.
 */
esp_err_t lua_runtime_init(void);

/**
 * Start a FreeRTOS task that executes /spiffs/main.lua
 */
esp_err_t lua_runtime_start(void);

/**
 * Stop the running Lua task, destroy VM, recreate, re-execute main.lua
 */
esp_err_t lua_runtime_restart(void);

/**
 * Execute a Lua code snippet in the current VM.
 * @param code   Lua source to execute
 * @param result Output buffer for return value (as string)
 * @param max_len Size of result buffer
 */
esp_err_t lua_runtime_exec(const char *code, char *result, size_t max_len);

/**
 * Read a script from SPIFFS.
 * @param name    Script filename (e.g. "main.lua")
 * @param buf     Output buffer
 * @param max_len Size of buf
 */
esp_err_t lua_runtime_get_script(const char *name, char *buf, size_t max_len);

/**
 * Write/overwrite a script on SPIFFS.
 * @param name    Script filename
 * @param content Script source code
 * @param append  If true, append to existing file instead of overwrite
 */
esp_err_t lua_runtime_push_script(const char *name, const char *content, bool append);

/**
 * List all scripts on SPIFFS.
 * @param buf     Output buffer (newline-separated filenames)
 * @param max_len Size of buf
 */
esp_err_t lua_runtime_list_scripts(char *buf, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif // LUA_RUNTIME_H
