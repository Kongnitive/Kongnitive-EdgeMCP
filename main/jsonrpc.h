/*
 * JSON-RPC 2.0 Message Handler
 * 
 * Implements JSON-RPC 2.0 specification for MCP protocol
 */

#ifndef JSONRPC_H
#define JSONRPC_H

#include <stdbool.h>
#include <esp_err.h>
#include <cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * JSON-RPC message types
 */
typedef enum {
    JSONRPC_REQUEST,        // Request with ID
    JSONRPC_NOTIFICATION,   // Request without ID
    JSONRPC_RESPONSE,       // Successful response
    JSONRPC_ERROR          // Error response
} jsonrpc_message_type_t;

/**
 * JSON-RPC error codes
 */
typedef enum {
    JSONRPC_PARSE_ERROR = -32700,           // Invalid JSON
    JSONRPC_INVALID_REQUEST = -32600,       // Invalid JSON-RPC
    JSONRPC_METHOD_NOT_FOUND = -32601,      // Unknown method
    JSONRPC_INVALID_PARAMS = -32602,        // Invalid parameters
    JSONRPC_INTERNAL_ERROR = -32603,        // Internal error
    JSONRPC_SERVER_ERROR = -32000           // Server error (generic)
} jsonrpc_error_code_t;

/**
 * Parsed JSON-RPC message structure
 */
typedef struct {
    jsonrpc_message_type_t type;
    int id;                         // Request ID (0 for notifications)
    bool has_id;                    // Whether ID field is present
    char method[64];                // Method name
    cJSON *params;                  // Parameters object (owned by this struct)
    cJSON *result;                  // Result object (for responses)
    int error_code;                 // Error code (for errors)
    char error_message[128];        // Error message
} jsonrpc_message_t;

/**
 * Parse a JSON-RPC 2.0 message from JSON string
 * 
 * @param json_str Input JSON string
 * @param msg Output message structure (caller must call jsonrpc_message_cleanup)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t jsonrpc_parse_message(const char *json_str, jsonrpc_message_t *msg);

/**
 * Create a JSON-RPC 2.0 success response
 * 
 * @param id Request ID
 * @param result Result object (will be deep-copied)
 * @return JSON string (caller must free), or NULL on error
 */
char* jsonrpc_create_response(int id, cJSON *result);

/**
 * Create a JSON-RPC 2.0 error response
 * 
 * @param id Request ID (use 0 if unknown)
 * @param code Error code
 * @param message Error message
 * @return JSON string (caller must free), or NULL on error
 */
char* jsonrpc_create_error(int id, int code, const char *message);

/**
 * Cleanup a parsed JSON-RPC message
 * Frees any allocated cJSON objects
 * 
 * @param msg Message to cleanup
 */
void jsonrpc_message_cleanup(jsonrpc_message_t *msg);

#ifdef __cplusplus
}
#endif

#endif // JSONRPC_H
