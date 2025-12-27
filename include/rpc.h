/*
 * Bitcoin Echo — RPC Interface
 *
 * Minimal JSON-RPC interface for external access to the node.
 * Provides read operations and transaction broadcast capabilities.
 *
 * Supported methods:
 *   - getblockchaininfo: Chain state summary
 *   - getblock: Block data by hash
 *   - getblockhash: Block hash by height
 *   - getrawtransaction: Raw transaction data
 *   - sendrawtransaction: Broadcast transaction
 *   - getblocktemplate: Mining work (for external miners)
 *   - submitblock: Submit mined block
 *
 * Protocol:
 *   - HTTP POST to /
 *   - Content-Type: application/json
 *   - JSON-RPC 1.0 format (id, method, params)
 *
 * There are no administrative commands, no wallet operations,
 * no runtime reconfiguration. This is intentionally minimal.
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_RPC_H
#define ECHO_RPC_H

#include "echo_types.h"
#include "node.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ============================================================================
 * RPC CONFIGURATION
 * ============================================================================
 */

/**
 * Default RPC port (mainnet).
 * Testnet and regtest use different ports.
 */
#define RPC_DEFAULT_PORT 8332
#define RPC_DEFAULT_PORT_TESTNET 18332
#define RPC_DEFAULT_PORT_REGTEST 18443

/**
 * Maximum size of an RPC request body (16 KB).
 */
#define RPC_MAX_REQUEST_SIZE ((size_t)16 * 1024)

/**
 * Maximum size of an RPC response (16 MB for large blocks).
 */
#define RPC_MAX_RESPONSE_SIZE ((size_t)16 * 1024 * 1024)

/**
 * RPC server accept backlog.
 */
#define RPC_BACKLOG 5

/**
 * RPC request timeout in milliseconds.
 */
#define RPC_TIMEOUT_MS 30000

/*
 * ============================================================================
 * JSON-RPC ERROR CODES
 * ============================================================================
 * Standard JSON-RPC 2.0 error codes plus Bitcoin-specific codes.
 */

/* Standard JSON-RPC errors */
#define RPC_ERR_PARSE_ERROR (-32700)        /* Invalid JSON */
#define RPC_ERR_INVALID_REQUEST (-32600)    /* Not a valid request */
#define RPC_ERR_METHOD_NOT_FOUND (-32601)   /* Method doesn't exist */
#define RPC_ERR_INVALID_PARAMS (-32602)     /* Invalid method parameters */
#define RPC_ERR_INTERNAL_ERROR (-32603)     /* Internal server error */

/* Bitcoin RPC errors (-1 to -99 reserved for general errors) */
#define RPC_ERR_MISC (-1)                   /* Miscellaneous error */
#define RPC_ERR_TYPE (-3)                   /* Unexpected type */
#define RPC_ERR_INVALID_PARAMETER (-8)      /* Invalid parameter value */
#define RPC_ERR_DATABASE (-20)              /* Database error */
#define RPC_ERR_DESERIALIZATION (-22)       /* Deserialization error */
#define RPC_ERR_VERIFY (-25)                /* Verification error */

/* P2P client errors (-9 to -17) */
#define RPC_ERR_CLIENT_IN_WARMUP (-28)      /* Client still warming up */

/* Block errors */
#define RPC_ERR_BLOCK_NOT_FOUND (-5)        /* Block not found */

/* Transaction errors */
#define RPC_ERR_TX_NOT_FOUND (-5)           /* Transaction not found */
#define RPC_ERR_TX_ALREADY_IN_CHAIN (-27)   /* Transaction already confirmed */
#define RPC_ERR_TX_REJECTED (-26)           /* Transaction rejected */

/*
 * ============================================================================
 * JSON PARSING TYPES
 * ============================================================================
 * Minimal JSON representation for RPC parsing.
 */

/**
 * JSON value type.
 */
typedef enum {
  JSON_NULL,
  JSON_BOOL,
  JSON_NUMBER,
  JSON_STRING,
  JSON_ARRAY,
  JSON_OBJECT
} json_type_t;

/**
 * JSON value (simple representation).
 * Uses a union for different types, with type tag.
 */
typedef struct json_value json_value_t;

/**
 * JSON object member (key-value pair).
 */
typedef struct json_member {
  char *key;
  json_value_t *value;
  struct json_member *next;
} json_member_t;

/**
 * JSON array element.
 */
typedef struct json_array_elem {
  json_value_t *value;
  struct json_array_elem *next;
} json_array_elem_t;

/**
 * JSON value structure.
 */
struct json_value {
  json_type_t type;
  union {
    bool boolean;
    double number;
    char *string;
    json_member_t *object;     /* Linked list of members */
    json_array_elem_t *array;  /* Linked list of elements */
  } u;
};

/**
 * Parse a JSON string into a value tree.
 *
 * Parameters:
 *   json - JSON string to parse (null-terminated)
 *   out  - Output: parsed JSON value
 *
 * Returns:
 *   ECHO_OK on success, error code on failure.
 */
echo_result_t json_parse(const char *json, json_value_t **out);

/**
 * Free a JSON value tree.
 *
 * Parameters:
 *   value - JSON value to free (may be NULL)
 */
void json_free(json_value_t *value);

/**
 * Get a member from a JSON object by key.
 *
 * Parameters:
 *   obj - JSON object
 *   key - Member key to find
 *
 * Returns:
 *   Pointer to member value, or NULL if not found.
 */
json_value_t *json_object_get(const json_value_t *obj, const char *key);

/**
 * Get an array element by index.
 *
 * Parameters:
 *   arr   - JSON array
 *   index - Element index (0-based)
 *
 * Returns:
 *   Pointer to element value, or NULL if out of bounds.
 */
json_value_t *json_array_get(const json_value_t *arr, size_t index);

/**
 * Get the length of a JSON array.
 *
 * Parameters:
 *   arr - JSON array
 *
 * Returns:
 *   Number of elements, or 0 if not an array.
 */
size_t json_array_length(const json_value_t *arr);

/*
 * ============================================================================
 * JSON BUILDING (for responses)
 * ============================================================================
 */

/**
 * JSON string builder for constructing responses.
 */
typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} json_builder_t;

/**
 * Initialize a JSON builder.
 *
 * Parameters:
 *   builder - Builder to initialize
 */
void json_builder_init(json_builder_t *builder);

/**
 * Free a JSON builder.
 *
 * Parameters:
 *   builder - Builder to free
 */
void json_builder_free(json_builder_t *builder);

/**
 * Get the built JSON string.
 * The string is owned by the builder and freed with json_builder_free().
 *
 * Parameters:
 *   builder - Builder
 *
 * Returns:
 *   JSON string (null-terminated).
 */
const char *json_builder_str(const json_builder_t *builder);

/**
 * Append raw string to builder.
 */
echo_result_t json_builder_append(json_builder_t *builder, const char *str);

/**
 * Append a JSON string value (with escaping).
 */
echo_result_t json_builder_string(json_builder_t *builder, const char *str);

/**
 * Append a JSON number value.
 */
echo_result_t json_builder_number(json_builder_t *builder, double num);

/**
 * Append a JSON integer value.
 */
echo_result_t json_builder_int(json_builder_t *builder, int64_t num);

/**
 * Append a JSON unsigned integer value.
 */
echo_result_t json_builder_uint(json_builder_t *builder, uint64_t num);

/**
 * Append a JSON boolean value.
 */
echo_result_t json_builder_bool(json_builder_t *builder, bool value);

/**
 * Append JSON null.
 */
echo_result_t json_builder_null(json_builder_t *builder);

/**
 * Append a hex-encoded string (for hashes, raw data).
 */
echo_result_t json_builder_hex(json_builder_t *builder, const uint8_t *data,
                               size_t len);

/*
 * ============================================================================
 * RPC SERVER
 * ============================================================================
 */

/**
 * RPC server structure (opaque).
 */
typedef struct rpc_server rpc_server_t;

/**
 * RPC server configuration.
 */
typedef struct {
  uint16_t port;           /* Port to listen on */
  const char *bind_addr;   /* Address to bind (NULL = 127.0.0.1) */
} rpc_config_t;

/**
 * Initialize RPC configuration with defaults.
 *
 * Parameters:
 *   config - Configuration to initialize
 */
void rpc_config_init(rpc_config_t *config);

/**
 * Create an RPC server.
 *
 * Parameters:
 *   config - Server configuration
 *   node   - Node to serve (provides chain state access)
 *
 * Returns:
 *   Newly created server, or NULL on failure.
 */
rpc_server_t *rpc_server_create(const rpc_config_t *config, node_t *node);

/**
 * Start the RPC server (begin listening).
 *
 * Parameters:
 *   server - Server to start
 *
 * Returns:
 *   ECHO_OK on success, error code on failure.
 */
echo_result_t rpc_server_start(rpc_server_t *server);

/**
 * Process pending RPC requests (non-blocking).
 *
 * This should be called regularly from the main event loop.
 * It accepts new connections and processes pending requests.
 *
 * Parameters:
 *   server - Server to process
 *
 * Returns:
 *   ECHO_OK on success, error code on failure.
 */
echo_result_t rpc_server_process(rpc_server_t *server);

/**
 * Stop the RPC server.
 *
 * Parameters:
 *   server - Server to stop
 *
 * Returns:
 *   ECHO_OK on success, error code on failure.
 */
echo_result_t rpc_server_stop(rpc_server_t *server);

/**
 * Destroy an RPC server and free resources.
 *
 * Parameters:
 *   server - Server to destroy (may be NULL)
 */
void rpc_server_destroy(rpc_server_t *server);

/**
 * Check if the RPC server is running.
 *
 * Parameters:
 *   server - Server to check
 *
 * Returns:
 *   true if server is listening, false otherwise.
 */
bool rpc_server_is_running(const rpc_server_t *server);

/**
 * Get the RPC server's listen port.
 *
 * Parameters:
 *   server - Server to query
 *
 * Returns:
 *   Port number, or 0 if not running.
 */
uint16_t rpc_server_get_port(const rpc_server_t *server);

/*
 * ============================================================================
 * RPC REQUEST/RESPONSE
 * ============================================================================
 */

/**
 * RPC request structure.
 */
typedef struct {
  char *id;            /* Request ID (may be NULL) */
  char *method;        /* Method name */
  json_value_t *params; /* Parameters (array or object, may be NULL) */
} rpc_request_t;

/**
 * Parse an RPC request from JSON.
 *
 * Parameters:
 *   json - JSON string
 *   req  - Output: parsed request
 *
 * Returns:
 *   ECHO_OK on success, error code on failure.
 */
echo_result_t rpc_request_parse(const char *json, rpc_request_t *req);

/**
 * Free an RPC request.
 *
 * Parameters:
 *   req - Request to free
 */
void rpc_request_free(rpc_request_t *req);

/**
 * Build an RPC success response.
 *
 * Parameters:
 *   id      - Request ID (may be NULL)
 *   result  - Result JSON string
 *   builder - Output: response builder
 *
 * Returns:
 *   ECHO_OK on success.
 */
echo_result_t rpc_response_success(const char *id, const char *result,
                                   json_builder_t *builder);

/**
 * Build an RPC error response.
 *
 * Parameters:
 *   id      - Request ID (may be NULL)
 *   code    - Error code
 *   message - Error message
 *   builder - Output: response builder
 *
 * Returns:
 *   ECHO_OK on success.
 */
echo_result_t rpc_response_error(const char *id, int code, const char *message,
                                 json_builder_t *builder);

/*
 * ============================================================================
 * RPC METHOD HANDLERS
 * ============================================================================
 * Individual method implementations. These are called by the server
 * after parsing the request.
 */

/**
 * Handle getblockchaininfo RPC.
 *
 * Returns chain state information:
 *   - chain: network name
 *   - blocks: current height
 *   - headers: known header count
 *   - bestblockhash: tip block hash
 *   - difficulty: current difficulty
 *   - mediantime: median time past
 *   - verificationprogress: sync progress
 *   - chainwork: total accumulated work
 *
 * Parameters:
 *   node    - Node to query
 *   params  - Request parameters (unused)
 *   builder - Output: result builder
 *
 * Returns:
 *   ECHO_OK on success, error code on failure.
 */
echo_result_t rpc_getblockchaininfo(node_t *node, const json_value_t *params,
                                    json_builder_t *builder);

/**
 * Handle getblock RPC.
 *
 * Returns block data by hash.
 * Verbosity 0 (default): hex-encoded serialized block
 * Verbosity 1: JSON object with decoded block info
 * Verbosity 2: JSON object with decoded transactions
 *
 * Parameters:
 *   node    - Node to query
 *   params  - [blockhash, verbosity]
 *   builder - Output: result builder
 *
 * Returns:
 *   ECHO_OK on success, RPC error code on failure.
 */
echo_result_t rpc_getblock(node_t *node, const json_value_t *params,
                           json_builder_t *builder);

/**
 * Handle getblockhash RPC.
 *
 * Returns block hash at given height.
 *
 * Parameters:
 *   node    - Node to query
 *   params  - [height]
 *   builder - Output: result builder
 *
 * Returns:
 *   ECHO_OK on success, RPC error code on failure.
 */
echo_result_t rpc_getblockhash(node_t *node, const json_value_t *params,
                               json_builder_t *builder);

/**
 * Handle getrawtransaction RPC.
 *
 * Returns raw transaction data.
 * Searches mempool and confirmed transactions.
 *
 * Parameters:
 *   node    - Node to query
 *   params  - [txid, verbose, blockhash]
 *   builder - Output: result builder
 *
 * Returns:
 *   ECHO_OK on success, RPC error code on failure.
 */
echo_result_t rpc_getrawtransaction(node_t *node, const json_value_t *params,
                                    json_builder_t *builder);

/**
 * Handle sendrawtransaction RPC.
 *
 * Broadcasts a raw transaction to the network.
 *
 * Parameters:
 *   node    - Node to use
 *   params  - [hexstring, maxfeerate]
 *   builder - Output: result builder (txid on success)
 *
 * Returns:
 *   ECHO_OK on success, RPC error code on failure.
 */
echo_result_t rpc_sendrawtransaction(node_t *node, const json_value_t *params,
                                     json_builder_t *builder);

/**
 * Handle getblocktemplate RPC.
 *
 * Returns data needed to construct a block for mining.
 *
 * Parameters:
 *   node    - Node to query
 *   params  - [template_request]
 *   builder - Output: result builder
 *
 * Returns:
 *   ECHO_OK on success, RPC error code on failure.
 */
echo_result_t rpc_getblocktemplate(node_t *node, const json_value_t *params,
                                   json_builder_t *builder);

/**
 * Handle submitblock RPC.
 *
 * Submits a mined block to the network.
 *
 * Parameters:
 *   node    - Node to use
 *   params  - [hexdata]
 *   builder - Output: result builder
 *
 * Returns:
 *   ECHO_OK on success, RPC error code on failure.
 */
echo_result_t rpc_submitblock(node_t *node, const json_value_t *params,
                              json_builder_t *builder);

/*
 * ============================================================================
 * UTILITY FUNCTIONS
 * ============================================================================
 */

/**
 * Parse a hex string into bytes.
 *
 * Parameters:
 *   hex     - Hex string (must be even length)
 *   out     - Output buffer
 *   out_len - Size of output buffer
 *   written - Output: bytes written
 *
 * Returns:
 *   ECHO_OK on success, error code on failure.
 */
echo_result_t rpc_hex_decode(const char *hex, uint8_t *out, size_t out_len,
                             size_t *written);

/**
 * Format a hash256 as a hex string (reversed for display).
 *
 * Parameters:
 *   hash - Hash to format
 *   out  - Output buffer (at least 65 bytes)
 */
void rpc_format_hash(const hash256_t *hash, char *out);

/**
 * Parse a hash from a hex string.
 *
 * Parameters:
 *   hex  - Hex string (64 characters)
 *   hash - Output: parsed hash
 *
 * Returns:
 *   ECHO_OK on success, error code on failure.
 */
echo_result_t rpc_parse_hash(const char *hex, hash256_t *hash);

#endif /* ECHO_RPC_H */
