/*
 * MCP Log Capture System Implementation
 *
 * Hooks into ESP-IDF logging via esp_log_set_vprintf() and stores
 * recent log lines in a ring buffer for retrieval by the sys_get_logs tool.
 */

#include "mcp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "sdkconfig.h"

#ifndef CONFIG_MCP_LOG_BUFFER_SIZE
#define CONFIG_MCP_LOG_BUFFER_SIZE 4096
#endif

#define LOG_LINE_MAX 256
#define LOG_MAX_LINES (CONFIG_MCP_LOG_BUFFER_SIZE / 64)

static const char *TAG = "mcp_log";

typedef struct {
    char text[LOG_LINE_MAX];
    esp_log_level_t level;
    int64_t timestamp_ms;
} log_entry_t;

static log_entry_t s_log_ring[LOG_MAX_LINES];
static int s_log_head = 0;       // next write index
static int s_log_count = 0;      // total entries stored
static SemaphoreHandle_t s_log_mutex = NULL;
static vprintf_like_t s_original_vprintf = NULL;

/* Detect log level from the ESP-IDF color-coded prefix character */
static esp_log_level_t detect_level_from_prefix(const char *str)
{
    if (!str || str[0] == '\0') return ESP_LOG_INFO;
    switch (str[0]) {
        case 'E': return ESP_LOG_ERROR;
        case 'W': return ESP_LOG_WARN;
        case 'I': return ESP_LOG_INFO;
        case 'D': return ESP_LOG_DEBUG;
        case 'V': return ESP_LOG_VERBOSE;
        default:  return ESP_LOG_INFO;
    }
}

/* Custom vprintf hook — captures log output into ring buffer */
static int log_vprintf_hook(const char *fmt, va_list args)
{
    /* Always forward to original output first */
    int ret = 0;
    if (s_original_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = s_original_vprintf(fmt, args_copy);
        va_end(args_copy);
    }

    /* Format the log line */
    char line[LOG_LINE_MAX];
    vsnprintf(line, sizeof(line), fmt, args);

    /* Strip trailing newline */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
    if (len == 0) return ret;

    /* Store in ring buffer */
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        log_entry_t *entry = &s_log_ring[s_log_head];
        strncpy(entry->text, line, LOG_LINE_MAX - 1);
        entry->text[LOG_LINE_MAX - 1] = '\0';
        entry->level = detect_level_from_prefix(line);
        entry->timestamp_ms = esp_timer_get_time() / 1000;

        s_log_head = (s_log_head + 1) % LOG_MAX_LINES;
        if (s_log_count < LOG_MAX_LINES) s_log_count++;

        xSemaphoreGive(s_log_mutex);
    }

    return ret;
}

esp_err_t mcp_log_init(void)
{
    s_log_mutex = xSemaphoreCreateMutex();
    if (!s_log_mutex) {
        return ESP_ERR_NO_MEM;
    }

    /* Hook into ESP-IDF logging */
    s_original_vprintf = esp_log_set_vprintf(log_vprintf_hook);
    ESP_LOGI(TAG, "Log capture initialized (ring buffer: %d entries)", LOG_MAX_LINES);
    return ESP_OK;
}

static esp_log_level_t parse_level_string(const char *level_str)
{
    if (!level_str) return ESP_LOG_INFO;
    if (strcmp(level_str, "error") == 0)   return ESP_LOG_ERROR;
    if (strcmp(level_str, "warn") == 0)    return ESP_LOG_WARN;
    if (strcmp(level_str, "info") == 0)    return ESP_LOG_INFO;
    if (strcmp(level_str, "debug") == 0)   return ESP_LOG_DEBUG;
    if (strcmp(level_str, "verbose") == 0) return ESP_LOG_VERBOSE;
    return ESP_LOG_INFO;
}

esp_err_t tool_sys_get_logs(cJSON *args, char *result, size_t max_len)
{
    /* Parse parameters */
    esp_log_level_t min_level = ESP_LOG_INFO;
    int max_lines = 20;
    const char *filter = NULL;

    if (args) {
        cJSON *level_item = cJSON_GetObjectItem(args, "level");
        if (level_item && cJSON_IsString(level_item)) {
            min_level = parse_level_string(level_item->valuestring);
        }
        cJSON *lines_item = cJSON_GetObjectItem(args, "lines");
        if (lines_item && cJSON_IsNumber(lines_item)) {
            max_lines = lines_item->valueint;
            if (max_lines < 1) max_lines = 1;
            if (max_lines > LOG_MAX_LINES) max_lines = LOG_MAX_LINES;
        }
        cJSON *filter_item = cJSON_GetObjectItem(args, "filter");
        if (filter_item && cJSON_IsString(filter_item)) {
            filter = filter_item->valuestring;
        }
    }

    if (!s_log_mutex) {
        snprintf(result, max_len, "Log system not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Collect matching entries */
    int written = 0;
    written += snprintf(result + written, max_len - written, "[");

    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int start = (s_log_count < LOG_MAX_LINES)
                    ? 0
                    : s_log_head;
        int found = 0;
        bool first = true;

        /* Walk ring buffer from oldest to newest, collect last max_lines matches */
        /* First pass: count matches to know where to start outputting */
        int match_count = 0;
        for (int i = 0; i < s_log_count; i++) {
            int idx = (start + i) % LOG_MAX_LINES;
            log_entry_t *e = &s_log_ring[idx];
            if (e->level > min_level) continue;
            if (filter && strstr(e->text, filter) == NULL) continue;
            match_count++;
        }

        int skip = (match_count > max_lines) ? (match_count - max_lines) : 0;

        for (int i = 0; i < s_log_count; i++) {
            int idx = (start + i) % LOG_MAX_LINES;
            log_entry_t *e = &s_log_ring[idx];

            if (e->level > min_level) continue;
            if (filter && strstr(e->text, filter) == NULL) continue;

            if (skip > 0) { skip--; continue; }

            /* Escape the text for JSON */
            if (!first) {
                written += snprintf(result + written, max_len - written, ",");
            }
            first = false;

            /* Simple JSON object per entry */
            written += snprintf(result + written, max_len - written,
                "{\"t\":%lld,\"msg\":\"", (long long)e->timestamp_ms);

            /* Escape special chars in message */
            for (const char *p = e->text; *p && written < (int)(max_len - 20); p++) {
                if (*p == '"') {
                    result[written++] = '\\';
                    result[written++] = '"';
                } else if (*p == '\\') {
                    result[written++] = '\\';
                    result[written++] = '\\';
                } else if (*p == '\n') {
                    result[written++] = '\\';
                    result[written++] = 'n';
                } else {
                    result[written++] = *p;
                }
            }
            written += snprintf(result + written, max_len - written, "\"}");

            found++;
            if (written >= (int)(max_len - 100)) break;
        }

        xSemaphoreGive(s_log_mutex);
    }

    written += snprintf(result + written, max_len - written, "]");
    result[max_len - 1] = '\0';

    return ESP_OK;
}
