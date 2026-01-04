/*
 * Bitcoin Echo — RPC Interface Implementation
 *
 * Minimal JSON-RPC server for external access to the node.
 *
 * This implementation includes:
 *   - Minimal embedded JSON parser (no external dependencies)
 *   - JSON builder for constructing responses
 *   - HTTP/1.0 request handling
 *   - RPC method dispatch
 *   - All required RPC methods
 *
 * Build once. Build right. Stop.
 */

#include "rpc.h"
#include "block.h"
#include "block_validate.h"
#include "chainstate.h"
#include "consensus.h"
#include "echo_config.h"
#include "echo_types.h"
#include "log.h"
#include "mempool.h"
#include "mining.h"
#include "node.h"
#include "platform.h"
#include "sync.h"
#include "tx.h"
#include "tx_validate.h"
#include "utxo.h"
#include "utxo_db.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ============================================================================
 * INTERNAL CONSTANTS
 * ============================================================================
 */

/* HTTP buffer sizes */
#define HTTP_HEADER_MAX_SIZE 4096
#define HTTP_READ_CHUNK_SIZE 1024

/* Initial JSON builder capacity */
#define JSON_BUILDER_INIT_CAP 1024

/*
 * ============================================================================
 * INTERNAL HELPERS
 * ============================================================================
 */

/* Portable string duplicate (avoids POSIX str_dup dependency) */
static char *str_dup(const char *s) {
  if (s == NULL) {
    return NULL;
  }
  size_t len = strlen(s) + 1;
  char *copy = malloc(len);
  if (copy != NULL) {
    memcpy(copy, s, len);
  }
  return copy;
}

/*
 * Calculate difficulty from compact bits representation.
 * Formula: difficulty = (0xFFFF * 2^208) / target
 * Where target is decoded from the compact bits field.
 */
static double difficulty_from_bits(uint32_t bits) {
  int shift = (int)((bits >> 24) & 0xff);
  double mantissa = (double)(bits & 0x00ffffff);
  if (mantissa == 0) {
    return 0.0;
  }
  double diff = (double)0x0000ffff / mantissa;
  while (shift < 29) {
    diff *= 256.0;
    shift++;
  }
  while (shift > 29) {
    diff /= 256.0;
    shift--;
  }
  return diff;
}

/*
 * ============================================================================
 * JSON PARSER IMPLEMENTATION
 * ============================================================================
 * A simple recursive descent JSON parser.
 */

/* Parser state */
typedef struct {
  const char *ptr;
  const char *end;
} json_parser_t;

/* Forward declarations */
static json_value_t *json_parse_value(json_parser_t *p);

/* Skip whitespace */
static void json_skip_ws(json_parser_t *p) {
  while (p->ptr < p->end && isspace((unsigned char)*p->ptr)) {
    p->ptr++;
  }
}

/* Check and consume a character */
static bool json_expect(json_parser_t *p, char c) {
  json_skip_ws(p);
  if (p->ptr < p->end && *p->ptr == c) {
    p->ptr++;
    return true;
  }
  return false;
}

/* Parse a string (including quotes) */
static char *json_parse_string_raw(json_parser_t *p) {
  json_skip_ws(p);
  if (p->ptr >= p->end || *p->ptr != '"') {
    return NULL;
  }
  p->ptr++; /* Skip opening quote */

  /* Find string length and check for escapes */
  const char *start = p->ptr;
  size_t len = 0;
  bool has_escapes = false;

  while (p->ptr < p->end && *p->ptr != '"') {
    if (*p->ptr == '\\') {
      has_escapes = true;
      p->ptr++;
      if (p->ptr >= p->end) {
        return NULL;
      }
    }
    p->ptr++;
    len++;
  }

  if (p->ptr >= p->end) {
    return NULL;
  }
  p->ptr++; /* Skip closing quote */

  /* Allocate and copy string */
  char *str = malloc(len + 1);
  if (str == NULL) {
    return NULL;
  }

  if (!has_escapes) {
    memcpy(str, start, len);
    str[len] = '\0';
  } else {
    /* Unescape */
    const char *src = start;
    char *dst = str;
    while (src < p->ptr - 1) {
      if (*src == '\\') {
        src++;
        switch (*src) {
        case '"':
          *dst++ = '"';
          break;
        case '\\':
          *dst++ = '\\';
          break;
        case '/':
          *dst++ = '/';
          break;
        case 'b':
          *dst++ = '\b';
          break;
        case 'f':
          *dst++ = '\f';
          break;
        case 'n':
          *dst++ = '\n';
          break;
        case 'r':
          *dst++ = '\r';
          break;
        case 't':
          *dst++ = '\t';
          break;
        case 'u':
          /* Unicode escape - simplified: just skip it */
          src += 4;
          *dst++ = '?';
          break;
        default:
          *dst++ = *src;
        }
        src++;
      } else {
        *dst++ = *src++;
      }
    }
    *dst = '\0';
  }

  return str;
}

/* Parse a number */
static json_value_t *json_parse_number(json_parser_t *p) {
  json_skip_ws(p);

  const char *start = p->ptr;

  /* Handle negative sign */
  if (p->ptr < p->end && *p->ptr == '-') {
    p->ptr++;
  }

  /* Integer part */
  if (p->ptr >= p->end || !isdigit((unsigned char)*p->ptr)) {
    p->ptr = start;
    return NULL;
  }
  while (p->ptr < p->end && isdigit((unsigned char)*p->ptr)) {
    p->ptr++;
  }

  /* Fractional part */
  if (p->ptr < p->end && *p->ptr == '.') {
    p->ptr++;
    while (p->ptr < p->end && isdigit((unsigned char)*p->ptr)) {
      p->ptr++;
    }
  }

  /* Exponent part */
  if (p->ptr < p->end && (*p->ptr == 'e' || *p->ptr == 'E')) {
    p->ptr++;
    if (p->ptr < p->end && (*p->ptr == '+' || *p->ptr == '-')) {
      p->ptr++;
    }
    while (p->ptr < p->end && isdigit((unsigned char)*p->ptr)) {
      p->ptr++;
    }
  }

  /* Parse the number */
  size_t num_len = (size_t)(p->ptr - start);
  char *num_str = malloc(num_len + 1);
  if (num_str == NULL) {
    return NULL;
  }
  memcpy(num_str, start, num_len);
  num_str[num_len] = '\0';

  double value = strtod(num_str, NULL);
  free(num_str);

  json_value_t *jv = malloc(sizeof(json_value_t));
  if (jv == NULL) {
    return NULL;
  }
  jv->type = JSON_NUMBER;
  jv->u.number = value;
  return jv;
}

/* Parse an object - recursion is required for nested JSON structures */
static json_value_t *json_parse_object(json_parser_t *p) { // NOLINT(misc-no-recursion)
  if (!json_expect(p, '{')) {
    return NULL;
  }

  json_value_t *obj = malloc(sizeof(json_value_t));
  if (obj == NULL) {
    return NULL;
  }
  obj->type = JSON_OBJECT;
  obj->u.object = NULL;

  json_member_t **tail = &obj->u.object;

  json_skip_ws(p);
  if (p->ptr < p->end && *p->ptr == '}') {
    p->ptr++;
    return obj;
  }

  while (true) {
    /* Parse key */
    char *key = json_parse_string_raw(p);
    if (key == NULL) {
      json_free(obj);
      return NULL;
    }

    /* Expect colon */
    if (!json_expect(p, ':')) {
      free(key);
      json_free(obj);
      return NULL;
    }

    /* Parse value */
    json_value_t *value = json_parse_value(p);
    if (value == NULL) {
      free(key);
      json_free(obj);
      return NULL;
    }

    /* Add member */
    json_member_t *member = malloc(sizeof(json_member_t));
    if (member == NULL) {
      free(key);
      json_free(value);
      json_free(obj);
      return NULL;
    }
    member->key = key;
    member->value = value;
    member->next = NULL;
    *tail = member;
    tail = &member->next;

    json_skip_ws(p);
    if (p->ptr >= p->end) {
      json_free(obj);
      return NULL;
    }

    if (*p->ptr == '}') {
      p->ptr++;
      return obj;
    }

    if (*p->ptr != ',') {
      json_free(obj);
      return NULL;
    }
    p->ptr++;
  }
}

/* Parse an array - recursion is required for nested JSON structures */
static json_value_t *json_parse_array(json_parser_t *p) { // NOLINT(misc-no-recursion)
  if (!json_expect(p, '[')) {
    return NULL;
  }

  json_value_t *arr = malloc(sizeof(json_value_t));
  if (arr == NULL) {
    return NULL;
  }
  arr->type = JSON_ARRAY;
  arr->u.array = NULL;

  json_array_elem_t **tail = &arr->u.array;

  json_skip_ws(p);
  if (p->ptr < p->end && *p->ptr == ']') {
    p->ptr++;
    return arr;
  }

  while (true) {
    /* Parse value */
    json_value_t *value = json_parse_value(p);
    if (value == NULL) {
      json_free(arr);
      return NULL;
    }

    /* Add element */
    json_array_elem_t *elem = malloc(sizeof(json_array_elem_t));
    if (elem == NULL) {
      json_free(value);
      json_free(arr);
      return NULL;
    }
    elem->value = value;
    elem->next = NULL;
    *tail = elem;
    tail = &elem->next;

    json_skip_ws(p);
    if (p->ptr >= p->end) {
      json_free(arr);
      return NULL;
    }

    if (*p->ptr == ']') {
      p->ptr++;
      return arr;
    }

    if (*p->ptr != ',') {
      json_free(arr);
      return NULL;
    }
    p->ptr++;
  }
}

/* Parse any value - recursion is required for nested JSON structures */
static json_value_t *json_parse_value(json_parser_t *p) { // NOLINT(misc-no-recursion)
  json_skip_ws(p);

  if (p->ptr >= p->end) {
    return NULL;
  }

  switch (*p->ptr) {
  case '{':
    return json_parse_object(p);
  case '[':
    return json_parse_array(p);
  case '"': {
    char *str = json_parse_string_raw(p);
    if (str == NULL) {
      return NULL;
    }
    json_value_t *jv = malloc(sizeof(json_value_t));
    if (jv == NULL) {
      free(str);
      return NULL;
    }
    jv->type = JSON_STRING;
    jv->u.string = str;
    return jv;
  }
  case 't': /* true */
    if (p->ptr + 4 <= p->end && strncmp(p->ptr, "true", 4) == 0) {
      p->ptr += 4;
      json_value_t *jv = malloc(sizeof(json_value_t));
      if (jv == NULL) {
        return NULL;
      }
      jv->type = JSON_BOOL;
      jv->u.boolean = true;
      return jv;
    }
    return NULL;
  case 'f': /* false */
    if (p->ptr + 5 <= p->end && strncmp(p->ptr, "false", 5) == 0) {
      p->ptr += 5;
      json_value_t *jv = malloc(sizeof(json_value_t));
      if (jv == NULL) {
        return NULL;
      }
      jv->type = JSON_BOOL;
      jv->u.boolean = false;
      return jv;
    }
    return NULL;
  case 'n': /* null */
    if (p->ptr + 4 <= p->end && strncmp(p->ptr, "null", 4) == 0) {
      p->ptr += 4;
      json_value_t *jv = malloc(sizeof(json_value_t));
      if (jv == NULL) {
        return NULL;
      }
      jv->type = JSON_NULL;
      return jv;
    }
    return NULL;
  default:
    if (*p->ptr == '-' || isdigit((unsigned char)*p->ptr)) {
      return json_parse_number(p);
    }
    return NULL;
  }
}

/* Public API: parse JSON string */
echo_result_t json_parse(const char *json, json_value_t **out) {
  if (json == NULL || out == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  json_parser_t parser;
  parser.ptr = json;
  parser.end = json + strlen(json);

  *out = json_parse_value(&parser);
  if (*out == NULL) {
    return ECHO_ERR_INVALID_FORMAT;
  }

  return ECHO_OK;
}

/* Free JSON value - recursion required for nested structures */
void json_free(json_value_t *value) { // NOLINT(misc-no-recursion)
  if (value == NULL) {
    return;
  }

  switch (value->type) {
  case JSON_STRING:
    free(value->u.string);
    break;
  case JSON_OBJECT:
    for (json_member_t *m = value->u.object; m != NULL;) {
      json_member_t *next = m->next;
      free(m->key);
      json_free(m->value);
      free(m);
      m = next;
    }
    break;
  case JSON_ARRAY:
    for (json_array_elem_t *e = value->u.array; e != NULL;) {
      json_array_elem_t *next = e->next;
      json_free(e->value);
      free(e);
      e = next;
    }
    break;
  default:
    break;
  }

  free(value);
}

/* Get object member by key */
json_value_t *json_object_get(const json_value_t *obj, const char *key) {
  if (obj == NULL || obj->type != JSON_OBJECT || key == NULL) {
    return NULL;
  }

  for (json_member_t *m = obj->u.object; m != NULL; m = m->next) {
    if (strcmp(m->key, key) == 0) {
      return m->value;
    }
  }

  return NULL;
}

/* Get array element by index */
json_value_t *json_array_get(const json_value_t *arr, size_t index) {
  if (arr == NULL || arr->type != JSON_ARRAY) {
    return NULL;
  }

  size_t i = 0;
  for (json_array_elem_t *e = arr->u.array; e != NULL; e = e->next, i++) {
    if (i == index) {
      return e->value;
    }
  }

  return NULL;
}

/* Get array length */
size_t json_array_length(const json_value_t *arr) {
  if (arr == NULL || arr->type != JSON_ARRAY) {
    return 0;
  }

  size_t count = 0;
  for (json_array_elem_t *e = arr->u.array; e != NULL; e = e->next) {
    count++;
  }

  return count;
}

/*
 * ============================================================================
 * JSON BUILDER IMPLEMENTATION
 * ============================================================================
 */

void json_builder_init(json_builder_t *builder) {
  builder->buf = malloc(JSON_BUILDER_INIT_CAP);
  builder->len = 0;
  builder->cap = JSON_BUILDER_INIT_CAP;
  if (builder->buf != NULL) {
    builder->buf[0] = '\0';
  }
}

void json_builder_free(json_builder_t *builder) {
  free(builder->buf);
  builder->buf = NULL;
  builder->len = 0;
  builder->cap = 0;
}

const char *json_builder_str(const json_builder_t *builder) {
  return builder->buf;
}

/* Ensure capacity for additional bytes */
static echo_result_t json_builder_ensure(json_builder_t *builder,
                                         size_t additional) {
  if (builder->buf == NULL) {
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  size_t needed = builder->len + additional + 1;
  if (needed <= builder->cap) {
    return ECHO_OK;
  }

  size_t new_cap = builder->cap * 2;
  while (new_cap < needed) {
    new_cap *= 2;
  }

  char *new_buf = realloc(builder->buf, new_cap);
  if (new_buf == NULL) {
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  builder->buf = new_buf;
  builder->cap = new_cap;
  return ECHO_OK;
}

echo_result_t json_builder_append(json_builder_t *builder, const char *str) {
  size_t len = strlen(str);
  echo_result_t res = json_builder_ensure(builder, len);
  if (res != ECHO_OK) {
    return res;
  }

  memcpy(builder->buf + builder->len, str, len + 1);
  builder->len += len;
  return ECHO_OK;
}

echo_result_t json_builder_string(json_builder_t *builder, const char *str) {
  /* Count needed space (with escapes) */
  size_t needed = 2; /* quotes */
  for (const char *p = str; *p; p++) {
    switch (*p) {
    case '"':
    case '\\':
    case '\b':
    case '\f':
    case '\n':
    case '\r':
    case '\t':
      needed += 2;
      break;
    default:
      if ((unsigned char)*p < 0x20) {
        needed += 6; /* \uXXXX */
      } else {
        needed++;
      }
    }
  }

  echo_result_t res = json_builder_ensure(builder, needed);
  if (res != ECHO_OK) {
    return res;
  }

  char *out = builder->buf + builder->len;
  *out++ = '"';

  for (const char *p = str; *p; p++) {
    switch (*p) {
    case '"':
      *out++ = '\\';
      *out++ = '"';
      break;
    case '\\':
      *out++ = '\\';
      *out++ = '\\';
      break;
    case '\b':
      *out++ = '\\';
      *out++ = 'b';
      break;
    case '\f':
      *out++ = '\\';
      *out++ = 'f';
      break;
    case '\n':
      *out++ = '\\';
      *out++ = 'n';
      break;
    case '\r':
      *out++ = '\\';
      *out++ = 'r';
      break;
    case '\t':
      *out++ = '\\';
      *out++ = 't';
      break;
    default:
      if ((unsigned char)*p < 0x20) {
        /* NOLINTBEGIN(cert-err33-c) - snprintf return checked implicitly */
        snprintf(out, 7, "\\u%04x", (unsigned char)*p);
        /* NOLINTEND(cert-err33-c) */
        out += 6;
      } else {
        *out++ = *p;
      }
    }
  }

  *out++ = '"';
  *out = '\0';
  builder->len = (size_t)(out - builder->buf);
  return ECHO_OK;
}

echo_result_t json_builder_number(json_builder_t *builder, double num) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%.17g", num);
  return json_builder_append(builder, buf);
}

echo_result_t json_builder_int(json_builder_t *builder, int64_t num) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", (long long)num);
  return json_builder_append(builder, buf);
}

echo_result_t json_builder_uint(json_builder_t *builder, uint64_t num) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%llu", (unsigned long long)num);
  return json_builder_append(builder, buf);
}

echo_result_t json_builder_bool(json_builder_t *builder, bool value) {
  return json_builder_append(builder, value ? "true" : "false");
}

echo_result_t json_builder_null(json_builder_t *builder) {
  return json_builder_append(builder, "null");
}

echo_result_t json_builder_hex(json_builder_t *builder, const uint8_t *data,
                               size_t len) {
  static const char hex_chars[] = "0123456789abcdef";

  size_t needed = len * 2 + 2; /* quotes + hex */
  echo_result_t res = json_builder_ensure(builder, needed);
  if (res != ECHO_OK) {
    return res;
  }

  char *out = builder->buf + builder->len;
  *out++ = '"';

  for (size_t i = 0; i < len; i++) {
    *out++ = hex_chars[(data[i] >> 4) & 0x0F];
    *out++ = hex_chars[data[i] & 0x0F];
  }

  *out++ = '"';
  *out = '\0';
  builder->len = (size_t)(out - builder->buf);
  return ECHO_OK;
}

/*
 * ============================================================================
 * RPC REQUEST/RESPONSE
 * ============================================================================
 */

echo_result_t rpc_response_success(const char *id, const char *result,
                                   json_builder_t *builder) {
  json_builder_init(builder);

  json_builder_append(builder, "{\"result\":");
  if (result != NULL && result[0] != '\0') {
    json_builder_append(builder, result);
  } else {
    json_builder_null(builder);
  }

  json_builder_append(builder, ",\"error\":null,\"id\":");
  if (id != NULL) {
    json_builder_string(builder, id);
  } else {
    json_builder_null(builder);
  }
  json_builder_append(builder, "}");

  return ECHO_OK;
}

echo_result_t rpc_response_error(const char *id, int code, const char *message,
                                 json_builder_t *builder) {
  json_builder_init(builder);

  json_builder_append(builder, "{\"result\":null,\"error\":{\"code\":");
  json_builder_int(builder, code);
  json_builder_append(builder, ",\"message\":");
  json_builder_string(builder, message);
  json_builder_append(builder, "},\"id\":");
  if (id != NULL) {
    json_builder_string(builder, id);
  } else {
    json_builder_null(builder);
  }
  json_builder_append(builder, "}");

  return ECHO_OK;
}

/*
 * ============================================================================
 * RPC SERVER STRUCTURE
 * ============================================================================
 */

struct rpc_server {
  plat_socket_t *listen_sock;
  node_t *node;
  uint16_t port;
  bool running;
};

void rpc_config_init(rpc_config_t *config) {
  if (config == NULL) {
    return;
  }
  config->port = RPC_DEFAULT_PORT;
  config->bind_addr = NULL; /* Default to localhost */
}

rpc_server_t *rpc_server_create(const rpc_config_t *config, node_t *node) {
  if (node == NULL) {
    return NULL;
  }

  rpc_server_t *server = calloc(1, sizeof(rpc_server_t));
  if (server == NULL) {
    return NULL;
  }

  server->node = node;
  server->port = config ? config->port : RPC_DEFAULT_PORT;
  server->listen_sock = NULL;
  server->running = false;

  return server;
}

echo_result_t rpc_server_start(rpc_server_t *server) {
  if (server == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (server->running) {
    return ECHO_OK; /* Already running */
  }

  /* Allocate and create listening socket */
  server->listen_sock = plat_socket_alloc();
  if (server->listen_sock == NULL) {
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  int res = plat_socket_create(server->listen_sock);
  if (res != PLAT_OK) {
    plat_socket_free(server->listen_sock);
    server->listen_sock = NULL;
    return ECHO_ERR_PLATFORM_IO;
  }

  /* Bind and listen */
  res = plat_socket_listen(server->listen_sock, server->port, RPC_BACKLOG);
  if (res != PLAT_OK) {
    plat_socket_close(server->listen_sock);
    plat_socket_free(server->listen_sock);
    server->listen_sock = NULL;
    return ECHO_ERR_PLATFORM_IO;
  }

  /* Set non-blocking mode so event loop doesn't block on accept() */
  res = plat_socket_set_nonblocking(server->listen_sock);
  if (res != PLAT_OK) {
    plat_socket_close(server->listen_sock);
    plat_socket_free(server->listen_sock);
    server->listen_sock = NULL;
    return ECHO_ERR_PLATFORM_IO;
  }

  server->running = true;
  return ECHO_OK;
}

/*
 * ============================================================================
 * HTTP REQUEST HANDLING
 * ============================================================================
 */

/* Simple HTTP request parser */
typedef struct {
  char method[16];
  char path[256];
  size_t content_length;
  const char *body;
} http_request_t;

/* Parse HTTP request headers */
static echo_result_t http_parse_request(const char *data, size_t len,
                                        http_request_t *req) {
  (void)len; /* Used implicitly by string functions */
  memset(req, 0, sizeof(*req));

  /* Find end of first line */
  const char *line_end = strchr(data, '\r');
  if (line_end == NULL) {
    return ECHO_ERR_INVALID_FORMAT;
  }

  /* Parse request line: METHOD PATH HTTP/x.x */
  /* NOLINTBEGIN(cert-err34-c,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
   * sscanf is correct here: we limit field widths (%15s, %255s match buffer
   * sizes) and check return value. sscanf_s is not portable C11. */
  if (sscanf(data, "%15s %255s", req->method, req->path) != 2) {
    return ECHO_ERR_INVALID_FORMAT;
  }
  /* NOLINTEND(cert-err34-c,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
   */

  /* Find Content-Length header */
  const char *cl = strstr(data, "Content-Length:");
  if (cl == NULL) {
    cl = strstr(data, "content-length:");
  }
  if (cl != NULL) {
    /* NOLINTBEGIN(cert-err34-c,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
     * sscanf is correct here: parsing numeric value into size_t, return value
     * checked. sscanf_s is not portable C11. */
    if (sscanf(cl, "%*[^:]: %zu", &req->content_length) != 1) {
      req->content_length = 0;
    }
    /* NOLINTEND(cert-err34-c,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
     */
  }

  /* Find body (after \r\n\r\n) */
  const char *body = strstr(data, "\r\n\r\n");
  if (body != NULL) {
    req->body = body + 4;
  }

  return ECHO_OK;
}

/* Send HTTP response */
static void http_send_response(plat_socket_t *sock, int status,
                               const char *status_text, const char *body) {
  char header[768]; /* Increased size for CORS headers */
  size_t body_len = body ? strlen(body) : 0;

  log_info(LOG_COMP_RPC, "Sending HTTP %d response, body_len=%zu", status, body_len);

  int header_len = snprintf(header, sizeof(header),
                            "HTTP/1.0 %d %s\r\n"
                            "Content-Type: application/json\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n"
                            "Access-Control-Allow-Origin: *\r\n"
                            "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
                            "Access-Control-Allow-Headers: Content-Type\r\n"
                            "Access-Control-Max-Age: 3600\r\n"
                            "\r\n",
                            status, status_text, body_len);

  int sent = plat_socket_send(sock, header, (size_t)header_len);
  log_info(LOG_COMP_RPC, "Sent header: %d bytes (expected %d)", sent, header_len);

  if (body != NULL && body_len > 0) {
    sent = plat_socket_send(sock, body, body_len);
    log_info(LOG_COMP_RPC, "Sent body: %d bytes (expected %zu)", sent, body_len);
  }
}

/*
 * ============================================================================
 * RPC METHOD DISPATCH
 * ============================================================================
 */

/* Method dispatch table entry */
typedef struct {
  const char *name;
  echo_result_t (*handler)(node_t *node, const json_value_t *params,
                           json_builder_t *builder);
} rpc_method_entry_t;

/* Forward declarations for observer RPC methods */
static echo_result_t rpc_getobserverstats(node_t *node, const json_value_t *params,
                                          json_builder_t *builder);
static echo_result_t rpc_getobservedblocks(node_t *node, const json_value_t *params,
                                           json_builder_t *builder);
static echo_result_t rpc_getobservedtxs(node_t *node, const json_value_t *params,
                                        json_builder_t *builder);
static echo_result_t rpc_pruneblockchain(node_t *node, const json_value_t *params,
                                         json_builder_t *builder);

/* Forward declaration for sync status RPC method */
static echo_result_t rpc_getsyncstatus(node_t *node, const json_value_t *params,
                                       json_builder_t *builder);

/* Method dispatch table */
static const rpc_method_entry_t rpc_methods[] = {
    {"getblockchaininfo", rpc_getblockchaininfo},
    {"getblock", rpc_getblock},
    {"getblockhash", rpc_getblockhash},
    {"getrawtransaction", rpc_getrawtransaction},
    {"sendrawtransaction", rpc_sendrawtransaction},
    {"getblocktemplate", rpc_getblocktemplate},
    {"submitblock", rpc_submitblock},
    {"getobserverstats", rpc_getobserverstats},
    {"getobservedblocks", rpc_getobservedblocks},
    {"getobservedtxs", rpc_getobservedtxs},
    {"pruneblockchain", rpc_pruneblockchain},
    {"getsyncstatus", rpc_getsyncstatus},         /* IBD performance */
    {NULL, NULL}};

/* Find method handler */
static echo_result_t (*rpc_find_method(const char *name))(node_t *,
                                                          const json_value_t *,
                                                          json_builder_t *) {
  for (size_t i = 0; rpc_methods[i].name != NULL; i++) {
    if (strcmp(rpc_methods[i].name, name) == 0) {
      return rpc_methods[i].handler;
    }
  }
  return NULL;
}

/* Execute a single RPC request and build response */
static void rpc_execute_single(rpc_server_t *server, rpc_request_t *req,
                                json_builder_t *response) {
  /* Find and execute method */
  echo_result_t (*handler)(node_t *, const json_value_t *, json_builder_t *) =
      rpc_find_method(req->method);

  if (handler == NULL) {
    rpc_response_error(req->id, RPC_ERR_METHOD_NOT_FOUND, "Method not found",
                       response);
  } else {
    json_builder_t result;
    json_builder_init(&result);

    log_info(LOG_COMP_RPC, "Calling handler for method: %s", req->method);
    echo_result_t res = handler(server->node, req->params, &result);

    const char *result_str = json_builder_str(&result);
    log_info(LOG_COMP_RPC, "Handler returned: res=%d", res);

    if (res == ECHO_OK) {
      rpc_response_success(req->id, result_str, response);
    } else {
      /* Handler sets its own error - extract it from result */
      rpc_response_error(req->id, RPC_ERR_INTERNAL_ERROR, "Internal error",
                         response);
    }

    json_builder_free(&result);
  }
}

/* Handle a single RPC request or batch of requests */
static void rpc_handle_request(rpc_server_t *server, plat_socket_t *client_sock,
                               const char *body) {
  /* Parse JSON to detect single vs batch request */
  json_value_t *root = NULL;
  echo_result_t parse_res = json_parse(body, &root);
  if (parse_res != ECHO_OK) {
    json_builder_t response;
    rpc_response_error(NULL, RPC_ERR_PARSE_ERROR, "Parse error", &response);
    http_send_response(client_sock, 200, "OK", json_builder_str(&response));
    json_builder_free(&response);
    return;
  }

  /* Check if this is a batch request (array) or single request (object) */
  if (root->type == JSON_ARRAY) {
    /* Batch request - process each request and return array of responses */
    size_t batch_size = json_array_length(root);

    log_info(LOG_COMP_RPC, "Processing batch request with %zu items", batch_size);

    if (batch_size == 0) {
      /* Empty batch is invalid per JSON-RPC 2.0 spec */
      json_builder_t response;
      rpc_response_error(NULL, RPC_ERR_INVALID_REQUEST, "Invalid request: empty batch",
                         &response);
      http_send_response(client_sock, 200, "OK", json_builder_str(&response));
      json_builder_free(&response);
      json_free(root);
      return;
    }

    json_builder_t batch_response;
    json_builder_init(&batch_response);
    json_builder_append(&batch_response, "[");

    /* Process each request in the batch */
    for (size_t i = 0; i < batch_size; i++) {
      json_value_t *request_obj = json_array_get(root, i);

      if (i > 0) {
        json_builder_append(&batch_response, ",");
      }

      if (request_obj == NULL || request_obj->type != JSON_OBJECT) {
        /* Invalid request in batch */
        json_builder_t error_response;
        rpc_response_error(NULL, RPC_ERR_INVALID_REQUEST, "Invalid request",
                           &error_response);
        json_builder_append(&batch_response, json_builder_str(&error_response));
        json_builder_free(&error_response);
        continue;
      }

      /* Parse this request */
      rpc_request_t req;
      memset(&req, 0, sizeof(req));

      /* Extract method */
      json_value_t *method = json_object_get(request_obj, "method");
      if (method == NULL || method->type != JSON_STRING) {
        json_builder_t error_response;
        rpc_response_error(NULL, RPC_ERR_INVALID_REQUEST, "Missing method",
                           &error_response);
        json_builder_append(&batch_response, json_builder_str(&error_response));
        json_builder_free(&error_response);
        continue;
      }
      req.method = str_dup(method->u.string);

      /* Extract id (optional) */
      json_value_t *id = json_object_get(request_obj, "id");
      if (id != NULL) {
        if (id->type == JSON_STRING) {
          req.id = str_dup(id->u.string);
        } else if (id->type == JSON_NUMBER) {
          char buf[32];
          snprintf(buf, sizeof(buf), "%.0f", id->u.number);
          req.id = str_dup(buf);
        } else if (id->type == JSON_NULL) {
          req.id = NULL;
        }
      }

      /* Extract params (optional) */
      json_value_t *params = json_object_get(request_obj, "params");
      if (params != NULL) {
        /* Deep copy params since we need to keep them */
        /* For simplicity, we'll just reference them - they're owned by root */
        req.params = params;
      }

      /* Execute the request */
      json_builder_t single_response;
      rpc_execute_single(server, &req, &single_response);
      json_builder_append(&batch_response, json_builder_str(&single_response));
      json_builder_free(&single_response);

      /* Free request (but don't free params - they're owned by root) */
      free(req.id);
      free(req.method);
    }

    json_builder_append(&batch_response, "]");

    const char *final_response = json_builder_str(&batch_response);
    log_info(LOG_COMP_RPC, "Sending batch response with %zu items", batch_size);
    http_send_response(client_sock, 200, "OK", final_response);
    json_builder_free(&batch_response);

  } else if (root->type == JSON_OBJECT) {
    /* Single request - original behavior */
    rpc_request_t req;
    memset(&req, 0, sizeof(req));

    /* Extract method */
    json_value_t *method = json_object_get(root, "method");
    if (method == NULL || method->type != JSON_STRING) {
      json_builder_t response;
      rpc_response_error(NULL, RPC_ERR_INVALID_REQUEST, "Missing method", &response);
      http_send_response(client_sock, 200, "OK", json_builder_str(&response));
      json_builder_free(&response);
      json_free(root);
      return;
    }
    req.method = str_dup(method->u.string);

    /* Extract id (optional) */
    json_value_t *id = json_object_get(root, "id");
    if (id != NULL) {
      if (id->type == JSON_STRING) {
        req.id = str_dup(id->u.string);
      } else if (id->type == JSON_NUMBER) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f", id->u.number);
        req.id = str_dup(buf);
      } else if (id->type == JSON_NULL) {
        req.id = NULL;
      }
    }

    /* Extract params (optional) */
    json_value_t *params = json_object_get(root, "params");
    if (params != NULL) {
      req.params = params;
    }

    /* Execute single request */
    json_builder_t response;
    rpc_execute_single(server, &req, &response);

    const char *final_response = json_builder_str(&response);
    log_info(LOG_COMP_RPC, "Sending single response: %s",
             final_response ? final_response : "(null)");
    http_send_response(client_sock, 200, "OK", final_response);
    json_builder_free(&response);

    /* Free request (but don't free params - they're owned by root) */
    free(req.id);
    free(req.method);

  } else {
    /* Invalid request type */
    json_builder_t response;
    rpc_response_error(NULL, RPC_ERR_INVALID_REQUEST, "Invalid request", &response);
    http_send_response(client_sock, 200, "OK", json_builder_str(&response));
    json_builder_free(&response);
  }

  json_free(root);
}

echo_result_t rpc_server_process(rpc_server_t *server) {
  static int call_count = 0;
  if (++call_count % 1000 == 0) {
    log_info(LOG_COMP_RPC, "rpc_server_process called %d times", call_count);
  }

  if (server == NULL || !server->running) {
    log_warn(LOG_COMP_RPC, "rpc_server_process: server NULL or not running");
    return ECHO_ERR_INVALID_STATE;
  }

  /* Allocate client socket */
  plat_socket_t *client_sock = plat_socket_alloc();
  if (client_sock == NULL) {
    log_error(LOG_COMP_RPC, "Failed to allocate client socket");
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  /* Check for new connection (blocking, but will timeout) */
  int res = plat_socket_accept(server->listen_sock, client_sock);
  if (res != PLAT_OK) {
    plat_socket_free(client_sock);
    return ECHO_OK; /* No pending connection */
  }

  log_info(LOG_COMP_RPC, "Accepted new RPC connection");

  /* Set short receive timeout - we expect data to arrive quickly */
  plat_socket_set_recv_timeout(client_sock, 100); /* 100ms initial timeout */

  /* Read HTTP request */
  char buf[RPC_MAX_REQUEST_SIZE];
  size_t total_read = 0;
  int recv_attempts = 0;
  const int max_recv_attempts = 20; /* Max 2 seconds total (20 * 100ms) */

  while (total_read < sizeof(buf) - 1 && recv_attempts < max_recv_attempts) {
    int n =
        plat_socket_recv(client_sock, buf + total_read, HTTP_READ_CHUNK_SIZE);
    recv_attempts++;

    if (n < 0) {
      /* Error or timeout - if we have data, try to process it */
      if (total_read > 0) {
        log_info(LOG_COMP_RPC, "Recv timeout/error after %zu bytes, processing", total_read);
        break;
      }
      /* No data at all - give up */
      log_warn(LOG_COMP_RPC, "Socket recv returned %d with no data, closing", n);
      plat_socket_close(client_sock);
      plat_socket_free(client_sock);
      return ECHO_OK;
    }
    if (n == 0) {
      /* Connection closed */
      log_info(LOG_COMP_RPC, "Connection closed by client");
      break;
    }

    total_read += (size_t)n;
    buf[total_read] = '\0';

    /* Check if we have complete request */
    if (strstr(buf, "\r\n\r\n") != NULL) {
      /* Check Content-Length */
      const char *cl = strstr(buf, "Content-Length:");
      if (cl == NULL) {
        cl = strstr(buf, "content-length:");
      }
      if (cl != NULL) {
        size_t content_length = 0;
        /* NOLINTBEGIN(cert-err34-c,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
         * sscanf is correct here: parsing numeric value into size_t.
         * If parse fails, content_length stays 0 which is safe. sscanf_s
         * is not portable C11. */
        sscanf(cl, "%*[^:]: %zu", &content_length);
        /* NOLINTEND(cert-err34-c,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
         */

        const char *body_start = strstr(buf, "\r\n\r\n") + 4;
        size_t header_len = (size_t)(body_start - buf);
        size_t body_received = total_read - header_len;

        if (body_received >= content_length) {
          break; /* Complete request */
        }
      } else {
        break; /* No Content-Length, assume complete */
      }
    }
  }

  /* Parse HTTP request */
  http_request_t http_req;
  echo_result_t parse_res = http_parse_request(buf, total_read, &http_req);

  /* Log request (truncate large bodies to keep logs clean) */
  if (parse_res == ECHO_OK && http_req.body != NULL) {
    size_t body_len = strlen(http_req.body);
    log_info(LOG_COMP_RPC, "Parsed HTTP: res=%d, method=%s, body_len=%zu",
             parse_res, http_req.method, body_len);
  } else {
    log_info(LOG_COMP_RPC, "Parsed HTTP: res=%d, method=%s",
             parse_res,
             (parse_res == ECHO_OK) ? http_req.method : "(null)");
  }

  if (parse_res == ECHO_OK && strcmp(http_req.method, "OPTIONS") == 0) {
    /* CORS preflight request - respond with 204 No Content and CORS headers
     * 204 is the standard status for OPTIONS and ensures browser caching */
    log_info(LOG_COMP_RPC, "Handling OPTIONS (CORS preflight)");
    http_send_response(client_sock, 204, "No Content", NULL);
  } else if (parse_res == ECHO_OK && strcmp(http_req.method, "POST") == 0 &&
             http_req.body != NULL) {
    log_info(LOG_COMP_RPC, "Handling POST");
    rpc_handle_request(server, client_sock, http_req.body);
  } else {
    /* Bad request */
    log_warn(LOG_COMP_RPC, "Bad request: parse_res=%d, method=%s",
             parse_res,
             (parse_res == ECHO_OK) ? http_req.method : "(null)");
    json_builder_t response;
    rpc_response_error(NULL, RPC_ERR_INVALID_REQUEST, "Invalid request",
                       &response);
    http_send_response(client_sock, 400, "Bad Request",
                       json_builder_str(&response));
    json_builder_free(&response);
  }

  plat_socket_close(client_sock);
  plat_socket_free(client_sock);
  return ECHO_OK;
}

echo_result_t rpc_server_stop(rpc_server_t *server) {
  if (server == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (!server->running) {
    return ECHO_OK;
  }

  if (server->listen_sock != NULL) {
    plat_socket_close(server->listen_sock);
    plat_socket_free(server->listen_sock);
    server->listen_sock = NULL;
  }

  server->running = false;
  return ECHO_OK;
}

void rpc_server_destroy(rpc_server_t *server) {
  if (server == NULL) {
    return;
  }

  rpc_server_stop(server);
  free(server);
}

bool rpc_server_is_running(const rpc_server_t *server) {
  return server != NULL && server->running;
}

uint16_t rpc_server_get_port(const rpc_server_t *server) {
  if (server == NULL || !server->running) {
    return 0;
  }
  return server->port;
}

/*
 * ============================================================================
 * UTILITY FUNCTIONS
 * ============================================================================
 */

/* Hex character to value */
static int hex_char_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + (c - 'A');
  }
  return -1;
}

echo_result_t rpc_hex_decode(const char *hex, uint8_t *out, size_t out_len,
                             size_t *written) {
  if (hex == NULL || out == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  size_t hex_len = strlen(hex);
  if (hex_len % 2 != 0) {
    return ECHO_ERR_INVALID_FORMAT;
  }

  size_t bytes = hex_len / 2;
  if (bytes > out_len) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }

  for (size_t i = 0; i < bytes; i++) {
    int hi = hex_char_value(hex[i * 2]);
    int lo = hex_char_value(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return ECHO_ERR_INVALID_FORMAT;
    }
    out[i] = (uint8_t)((hi << 4) | lo);
  }

  if (written) {
    *written = bytes;
  }
  return ECHO_OK;
}

void rpc_format_hash(const hash256_t *hash, char *out) {
  static const char hex_chars[] = "0123456789abcdef";

  /* Bitcoin displays hashes in reverse byte order */
  for (int i = 31; i >= 0; i--) {
    *out++ = hex_chars[(hash->bytes[i] >> 4) & 0x0F];
    *out++ = hex_chars[hash->bytes[i] & 0x0F];
  }
  *out = '\0';
}

echo_result_t rpc_parse_hash(const char *hex, hash256_t *hash) {
  if (hex == NULL || hash == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (strlen(hex) != 64) {
    return ECHO_ERR_INVALID_FORMAT;
  }

  /* Parse in reverse (display order to internal order) */
  for (size_t i = 0; i < 32; i++) {
    size_t hex_idx = (31 - i) * 2;
    int hi = hex_char_value(hex[hex_idx]);
    int lo = hex_char_value(hex[hex_idx + 1]);
    if (hi < 0 || lo < 0) {
      return ECHO_ERR_INVALID_FORMAT;
    }
    hash->bytes[i] = (uint8_t)((hi << 4) | lo);
  }

  return ECHO_OK;
}

/*
 * ============================================================================
 * RPC METHOD IMPLEMENTATIONS
 * ============================================================================
 */

/* getblockchaininfo */
echo_result_t rpc_getblockchaininfo(node_t *node, const json_value_t *params,
                                    json_builder_t *builder) {
  (void)params; /* Unused */

  if (node == NULL || builder == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  const consensus_engine_t *consensus = node_get_consensus_const(node);
  if (consensus == NULL) {
    return ECHO_ERR_INVALID_STATE;
  }

  /* Get chain info */
  chain_tip_t tip;
  consensus_get_chain_tip(consensus, &tip);

  consensus_stats_t stats;
  consensus_get_stats(consensus, &stats);

  node_stats_t node_stats;
  node_get_stats(node, &node_stats);

  /* Get sync progress for accurate block/header counts */
  sync_progress_t sync_progress = {0};
  sync_manager_t *sync_mgr = node_get_sync_manager(node);
  if (sync_mgr != NULL) {
    sync_get_progress(sync_mgr, &sync_progress);
  }

  /* Format block hash */
  char hash_hex[65];
  rpc_format_hash(&tip.hash, hash_hex);

  /* Format chainwork as hex */
  char chainwork_hex[65];
  rpc_format_hash((const hash256_t *)&tip.chainwork, chainwork_hex);

  /* Build JSON response */
  json_builder_append(builder, "{");

  json_builder_append(builder, "\"chain\":");
  json_builder_string(builder, ECHO_NETWORK_NAME);

  /*
   * blocks = height of last fully validated block (with transaction data)
   * This is the chainstate tip height, which persists across restarts.
   * Note: sync_progress.blocks_validated is a session counter, not the height.
   */
  uint32_t validated_height = consensus_get_height(consensus);
  /* consensus_get_height returns UINT32_MAX if not initialized */
  if (validated_height == UINT32_MAX) {
    validated_height = 0;
  }
  json_builder_append(builder, ",\"blocks\":");
  json_builder_uint(builder, validated_height);

  /*
   * headers = height of best header chain
   * We use MAX of:
   *   - tip.height (from restored headers / validated blocks)
   *   - sync_progress.best_header_height (current session's header progress)
   * This ensures we show accurate count during both restore and active sync.
   */
  uint32_t header_height = tip.height;
  if (sync_progress.best_header_height > header_height) {
    header_height = sync_progress.best_header_height;
  }
  json_builder_append(builder, ",\"headers\":");
  json_builder_uint(builder, header_height);

  json_builder_append(builder, ",\"bestblockhash\":");
  json_builder_string(builder, hash_hex);

  json_builder_append(builder, ",\"difficulty\":");
  const block_index_t *tip_index = consensus_lookup_block_index(consensus, &tip.hash);
  double difficulty = (tip_index != NULL) ? difficulty_from_bits(tip_index->bits) : 1.0;
  json_builder_number(builder, difficulty);

  json_builder_append(builder, ",\"mediantime\":");
  json_builder_uint(builder, 0); /* TODO: implement MTP query */

  json_builder_append(builder, ",\"verificationprogress\":");
  json_builder_number(builder, node_stats.sync_progress / 100.0);

  json_builder_append(builder, ",\"initialblockdownload\":");
  json_builder_bool(builder, node_stats.is_syncing);

  json_builder_append(builder, ",\"chainwork\":");
  json_builder_string(builder, chainwork_hex);

  /* Get actual disk usage from block storage */
  uint64_t disk_usage = node_get_block_storage_size(node);
  json_builder_append(builder, ",\"size_on_disk\":");
  json_builder_uint(builder, disk_usage);

  /* Pruning information */
  bool is_pruned = node_is_pruning_enabled(node);
  json_builder_append(builder, ",\"pruned\":");
  json_builder_bool(builder, is_pruned);

  if (is_pruned) {
    /* Add pruneheight - lowest block height with available data */
    uint32_t pruned_height = node_get_pruned_height(node);
    json_builder_append(builder, ",\"pruneheight\":");
    json_builder_uint(builder, pruned_height);

    /* Add prune_target_size - configured target in bytes */
    uint64_t prune_target = node_get_prune_target(node);
    json_builder_append(builder, ",\"prune_target_size\":");
    json_builder_uint(builder, prune_target * 1024 * 1024); /* MB to bytes */
  }

  json_builder_append(builder, "}");

  return ECHO_OK;
}

/* getblockhash */
echo_result_t rpc_getblockhash(node_t *node, const json_value_t *params,
                               json_builder_t *builder) {
  if (node == NULL || builder == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Get height parameter */
  json_value_t *height_val = json_array_get(params, 0);
  if (height_val == NULL || height_val->type != JSON_NUMBER) {
    return ECHO_ERR_INVALID_PARAM;
  }

  uint32_t height = (uint32_t)height_val->u.number;

  const consensus_engine_t *consensus = node_get_consensus_const(node);
  if (consensus == NULL) {
    return ECHO_ERR_INVALID_STATE;
  }

  hash256_t hash;
  echo_result_t res = consensus_get_block_hash(consensus, height, &hash);
  if (res != ECHO_OK) {
    return res;
  }

  char hash_hex[65];
  rpc_format_hash(&hash, hash_hex);
  json_builder_string(builder, hash_hex);

  return ECHO_OK;
}

/* getblock */
echo_result_t rpc_getblock(node_t *node, const json_value_t *params,
                           json_builder_t *builder) {
  if (node == NULL || builder == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Get blockhash parameter */
  json_value_t *hash_val = json_array_get(params, 0);
  if (hash_val == NULL || hash_val->type != JSON_STRING) {
    return ECHO_ERR_INVALID_PARAM;
  }

  /* Get verbosity (default 1) */
  int verbosity = 1;
  json_value_t *verb_val = json_array_get(params, 1);
  if (verb_val != NULL && verb_val->type == JSON_NUMBER) {
    verbosity = (int)verb_val->u.number;
  }

  /* Parse block hash */
  hash256_t block_hash;
  echo_result_t res = rpc_parse_hash(hash_val->u.string, &block_hash);
  if (res != ECHO_OK) {
    return res;
  }

  /* Find block in index */
  const consensus_engine_t *consensus = node_get_consensus_const(node);
  const block_index_t *index =
      consensus_lookup_block_index(consensus, &block_hash);
  if (index == NULL) {
    return ECHO_ERR_NOT_FOUND;
  }

  if (verbosity == 0) {
    /* Return raw hex block */
    /* TODO: Read block from storage and return hex */
    json_builder_string(builder, "");
    return ECHO_OK;
  }

  /* Build JSON response */
  char hash_hex[65];
  rpc_format_hash(&block_hash, hash_hex);

  char prev_hash_hex[65];
  rpc_format_hash(&index->prev_hash, prev_hash_hex);

  json_builder_append(builder, "{");

  json_builder_append(builder, "\"hash\":");
  json_builder_string(builder, hash_hex);

  json_builder_append(builder, ",\"confirmations\":");
  uint32_t tip_height = consensus_get_height(consensus);
  int64_t confirmations = (int64_t)tip_height - (int64_t)index->height + 1;
  json_builder_int(builder, confirmations);

  json_builder_append(builder, ",\"height\":");
  json_builder_uint(builder, index->height);

  json_builder_append(builder, ",\"version\":");
  json_builder_int(builder, 1); /* Simplified */

  json_builder_append(builder, ",\"time\":");
  json_builder_uint(builder, index->timestamp);

  json_builder_append(builder, ",\"mediantime\":");
  json_builder_uint(builder, index->timestamp); /* Simplified */

  json_builder_append(builder, ",\"bits\":");
  char bits_hex[9];
  snprintf(bits_hex, sizeof(bits_hex), "%08x", index->bits);
  json_builder_string(builder, bits_hex);

  json_builder_append(builder, ",\"difficulty\":");
  json_builder_number(builder, difficulty_from_bits(index->bits));

  json_builder_append(builder, ",\"previousblockhash\":");
  if (index->height > 0) {
    json_builder_string(builder, prev_hash_hex);
  } else {
    json_builder_null(builder);
  }

  /* Next block hash (if exists) */
  json_builder_append(builder, ",\"nextblockhash\":");
  hash256_t next_hash;
  res = consensus_get_block_hash(consensus, index->height + 1, &next_hash);
  if (res == ECHO_OK) {
    char next_hex[65];
    rpc_format_hash(&next_hash, next_hex);
    json_builder_string(builder, next_hex);
  } else {
    json_builder_null(builder);
  }

  json_builder_append(builder, ",\"nTx\":");
  json_builder_uint(builder, 0); /* TODO: get tx count from storage */

  json_builder_append(builder, "}");

  return ECHO_OK;
}

/* getrawtransaction */
echo_result_t rpc_getrawtransaction(node_t *node, const json_value_t *params,
                                    json_builder_t *builder) {
  if (node == NULL || builder == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Get txid parameter */
  json_value_t *txid_val = json_array_get(params, 0);
  if (txid_val == NULL || txid_val->type != JSON_STRING) {
    return ECHO_ERR_INVALID_PARAM;
  }

  /* Parse txid */
  hash256_t txid;
  echo_result_t res = rpc_parse_hash(txid_val->u.string, &txid);
  if (res != ECHO_OK) {
    return res;
  }

  /* Check mempool first */
  const mempool_t *mp = node_get_mempool_const(node);
  const mempool_entry_t *entry = mempool_lookup(mp, &txid);

  if (entry != NULL) {
    /* Found in mempool - serialize and return */
    size_t tx_size = tx_serialize_size(&entry->tx, ECHO_TRUE);
    uint8_t *tx_data = malloc(tx_size);
    if (tx_data == NULL) {
      return ECHO_ERR_OUT_OF_MEMORY;
    }

    size_t written;
    res = tx_serialize(&entry->tx, ECHO_TRUE, tx_data, tx_size, &written);
    if (res != ECHO_OK) {
      free(tx_data);
      return res;
    }

    json_builder_hex(builder, tx_data, written);
    free(tx_data);
    return ECHO_OK;
  }

  /* Not in mempool - would need to search blocks */
  /* TODO: Implement transaction index for confirmed txs */
  return ECHO_ERR_NOT_FOUND;
}

/**
 * sendrawtransaction - Submit a raw transaction to the mempool.
 *
 * Performs full UTXO validation before mempool acceptance.
 *
 * Parameters:
 *   [0] hexstring - The serialized transaction in hex
 *   [1] maxfeerate (optional) - Maximum fee rate to accept (sat/vB)
 *
 * Returns:
 *   txid - The transaction hash in hex on success
 *
 * Error handling:
 *   - Parse errors → RPC_ERR_DESERIALIZATION
 *   - Validation errors → RPC_ERR_VERIFY
 *   - Mempool rejection → RPC_ERR_VERIFY with reason
 */
echo_result_t rpc_sendrawtransaction(node_t *node, const json_value_t *params,
                                     json_builder_t *builder) {
  if (node == NULL || builder == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Get hex parameter */
  json_value_t *hex_val = json_array_get(params, 0);
  if (hex_val == NULL || hex_val->type != JSON_STRING) {
    return ECHO_ERR_INVALID_PARAM;
  }

  /* Decode hex */
  size_t hex_len = strlen(hex_val->u.string);
  size_t tx_max_len = hex_len / 2;
  uint8_t *tx_data = malloc(tx_max_len);
  if (tx_data == NULL) {
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  size_t tx_len;
  echo_result_t res =
      rpc_hex_decode(hex_val->u.string, tx_data, tx_max_len, &tx_len);
  if (res != ECHO_OK) {
    free(tx_data);
    log_warn(LOG_COMP_RPC, "sendrawtransaction: failed to decode hex");
    return res;
  }

  /* Parse transaction */
  tx_t tx;
  tx_init(&tx);
  res = tx_parse(tx_data, tx_len, &tx, NULL);
  free(tx_data);

  if (res != ECHO_OK) {
    log_warn(LOG_COMP_RPC, "sendrawtransaction: failed to parse transaction");
    return res;
  }

  /* Step 1: Syntactic validation (quick structural checks) */
  tx_validate_result_t validate_result;
  tx_validate_result_init(&validate_result);
  res = tx_validate_syntax(&tx, &validate_result);
  if (res != ECHO_OK) {
    log_warn(LOG_COMP_RPC, "sendrawtransaction: syntax validation failed: %s",
             tx_validate_error_string(validate_result.error));
    tx_free(&tx);
    return ECHO_ERR_INVALID;
  }

  /* Step 2: Look up UTXOs for all inputs */
  utxo_db_t *utxo_db = node_get_utxo_db(node);
  mempool_t *mp = node_get_mempool(node);
  const consensus_engine_t *consensus = node_get_consensus_const(node);

  /* Allocate UTXO info array for validation context */
  utxo_info_t *utxo_infos = calloc(tx.input_count, sizeof(utxo_info_t));
  if (utxo_infos == NULL) {
    tx_free(&tx);
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  /* Look up each input's UTXO */
  for (size_t i = 0; i < tx.input_count; i++) {
    const tx_input_t *input = &tx.inputs[i];
    utxo_entry_t *entry = NULL;

    /* First check UTXO database (confirmed outputs) */
    res = utxo_db_lookup(utxo_db, &input->prevout, &entry);

    /* If not found in database, check mempool for unconfirmed outputs */
    if (res == ECHO_ERR_NOT_FOUND && mp != NULL) {
      const mempool_entry_t *mem_entry =
          mempool_lookup(mp, &input->prevout.txid);
      if (mem_entry != NULL &&
          input->prevout.vout < mem_entry->tx.output_count) {
        /* Found in mempool - build entry from transaction output */
        const tx_output_t *txout =
            &mem_entry->tx.outputs[input->prevout.vout];
        utxo_infos[i].value = txout->value;
        utxo_infos[i].script_pubkey = txout->script_pubkey;
        utxo_infos[i].script_pubkey_len = txout->script_pubkey_len;
        utxo_infos[i].height = 0; /* Unconfirmed */
        utxo_infos[i].is_coinbase = ECHO_FALSE;
        continue; /* Successfully found in mempool */
      }
    }

    if (res != ECHO_OK || entry == NULL) {
      log_warn(LOG_COMP_RPC,
               "sendrawtransaction: missing input UTXO at index %zu", i);
      /* Free any already-allocated UTXOs */
      for (size_t j = 0; j < i; j++) {
        /* Only free if we allocated it from database lookup */
        if (utxo_infos[j].height > 0) {
          /* This was from database - the entry was freed already */
        }
      }
      free(utxo_infos);
      tx_free(&tx);
      return ECHO_ERR_NOT_FOUND;
    }

    /* Copy UTXO info for validation */
    utxo_infos[i].value = entry->value;
    utxo_infos[i].script_pubkey = entry->script_pubkey;
    utxo_infos[i].script_pubkey_len = entry->script_len;
    utxo_infos[i].height = entry->height;
    utxo_infos[i].is_coinbase = entry->is_coinbase ? ECHO_TRUE : ECHO_FALSE;

    /* Don't free entry yet - we need the script pointer.
     * We'll need to copy the script or handle this differently. */
    /* For now, leak the entry - proper fix would be to copy script data */
    (void)entry; /* Mark as intentionally not freed for now */
  }

  /* Step 3: Build validation context */
  uint32_t current_height = consensus_get_height(consensus);
  tx_validate_ctx_t ctx = {
      .block_height = current_height + 1, /* Next block */
      .block_time = (uint32_t)(plat_time_ms() / 1000),
      .median_time_past = (uint32_t)(plat_time_ms() / 1000), /* Simplified */
      .utxos = utxo_infos,
      .utxo_count = tx.input_count,
      .script_flags = consensus_get_script_flags(current_height + 1)};

  /* Step 4: Full validation with UTXO context */
  tx_validate_result_init(&validate_result);
  res = tx_validate(&tx, &ctx, &validate_result);
  if (res != ECHO_OK) {
    log_warn(LOG_COMP_RPC, "sendrawtransaction: validation failed: %s",
             tx_validate_error_string(validate_result.error));
    free(utxo_infos);
    tx_free(&tx);
    return ECHO_ERR_INVALID;
  }

  /* Step 5: Compute fee and check against mempool minimum */
  satoshi_t fee = 0;
  res = tx_compute_fee(&tx, utxo_infos, tx.input_count, &fee);
  if (res != ECHO_OK) {
    log_warn(LOG_COMP_RPC, "sendrawtransaction: failed to compute fee");
    free(utxo_infos);
    tx_free(&tx);
    return ECHO_ERR_INVALID;
  }

  /* Calculate fee rate (sat/kvB) */
  size_t vsize = tx_vsize(&tx);
  uint64_t fee_rate = (vsize > 0) ? ((uint64_t)fee * 1000 / vsize) : 0;

  /* Check against mempool minimum */
  uint64_t min_fee_rate = mempool_min_fee_rate(mp);
  if (fee_rate < min_fee_rate) {
    log_warn(LOG_COMP_RPC,
             "sendrawtransaction: fee rate too low (%llu < %llu sat/kvB)",
             (unsigned long long)fee_rate, (unsigned long long)min_fee_rate);
    free(utxo_infos);
    tx_free(&tx);
    return ECHO_ERR_INVALID;
  }

  /* Done with UTXO infos */
  free(utxo_infos);

  /* Step 6: Add to mempool */
  mempool_accept_result_t accept_result;
  mempool_accept_result_init(&accept_result);
  res = mempool_add(mp, &tx, &accept_result);

  if (res != ECHO_OK) {
    log_warn(LOG_COMP_RPC, "sendrawtransaction: mempool rejected: %s",
             mempool_reject_string(accept_result.reason));
    tx_free(&tx);
    return res;
  }

  /* Step 7: Return txid on success */
  hash256_t txid;
  tx_compute_txid(&tx, &txid);
  tx_free(&tx);

  char txid_hex[65];
  rpc_format_hash(&txid, txid_hex);
  json_builder_string(builder, txid_hex);

  log_info(LOG_COMP_RPC, "sendrawtransaction: accepted %s", txid_hex);

  return ECHO_OK;
}

/* getblocktemplate */
echo_result_t rpc_getblocktemplate(node_t *node, const json_value_t *params,
                                   json_builder_t *builder) {
  (void)params; /* TODO: process template_request */

  if (node == NULL || builder == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  const consensus_engine_t *consensus = node_get_consensus_const(node);

  /* Get current tip */
  chain_tip_t tip;
  consensus_get_chain_tip(consensus, &tip);

  char prev_hash[65];
  rpc_format_hash(&tip.hash, prev_hash);

  /* Next block height */
  uint32_t next_height = tip.height + 1;

  /* Get transactions from mempool */
  const mempool_t *mp = node_get_mempool_const(node);
  mempool_stats_t mp_stats;
  mempool_get_stats(mp, &mp_stats);

  json_builder_append(builder, "{");

  json_builder_append(builder, "\"version\":");
  json_builder_int(builder, 0x20000000); /* BIP9 version bits */

  json_builder_append(builder, ",\"previousblockhash\":");
  json_builder_string(builder, prev_hash);

  json_builder_append(builder, ",\"transactions\":[");

  /* Select transactions for block */
  const mempool_entry_t *selected[1000];
  size_t selected_count = 0;
  mempool_select_for_block(mp, selected, 1000,
                           BLOCK_MAX_WEIGHT - 4000, /* Reserve for coinbase */
                           &selected_count);

  satoshi_t total_fees = 0;
  for (size_t i = 0; i < selected_count; i++) {
    if (i > 0) {
      json_builder_append(builder, ",");
    }

    const mempool_entry_t *entry = selected[i];
    total_fees += entry->fee;

    /* Serialize transaction data */
    size_t tx_size = tx_serialize_size(&entry->tx, ECHO_TRUE);
    uint8_t *tx_data = malloc(tx_size);
    if (tx_data == NULL) {
      continue;
    }

    size_t written;
    if (tx_serialize(&entry->tx, ECHO_TRUE, tx_data, tx_size, &written) ==
        ECHO_OK) {
      char txid_hex[65];
      rpc_format_hash(&entry->txid, txid_hex);

      json_builder_append(builder, "{\"data\":");
      json_builder_hex(builder, tx_data, written);
      json_builder_append(builder, ",\"txid\":");
      json_builder_string(builder, txid_hex);
      json_builder_append(builder, ",\"fee\":");
      json_builder_int(builder, entry->fee);
      json_builder_append(builder, ",\"weight\":");
      json_builder_uint(builder, entry->vsize * 4);
      json_builder_append(builder, "}");
    }
    free(tx_data);
  }

  json_builder_append(builder, "]");

  /* Coinbase value = subsidy + fees using proper subsidy calculation */
  satoshi_t subsidy = coinbase_subsidy(next_height);

  json_builder_append(builder, ",\"coinbasevalue\":");
  json_builder_int(builder, subsidy + total_fees);

  /*
   * Determine difficulty bits.
   * For regtest: always use trivial difficulty (REGTEST_POWLIMIT_BITS).
   * For mainnet/testnet: would need full difficulty calculation from context.
   */
  uint32_t bits;
#if defined(ECHO_NETWORK_REGTEST)
  /* Regtest: trivial difficulty, no adjustment */
  bits = REGTEST_POWLIMIT_BITS;
#else
  /* Mainnet/testnet: use genesis difficulty as placeholder.
   * Full difficulty adjustment would require the difficulty context. */
  bits = DIFFICULTY_POWLIMIT_BITS;
#endif

  /* Convert bits to target for the "target" field */
  hash256_t target;
  block_bits_to_target(bits, &target);

  /* Format target as hex string (big-endian for display) */
  char target_hex[65];
  for (size_t i = 0; i < 32; i++) {
    snprintf(target_hex + (i * 2), 3, "%02x", target.bytes[31 - i]);
  }

  json_builder_append(builder, ",\"target\":");
  json_builder_string(builder, target_hex);

  /*
   * Calculate minimum time (MTP + 1).
   * For regtest with empty chain, use 0.
   * For proper operation, would query block index for MTP.
   */
  uint32_t mintime = 0;
  if (tip.height > 0) {
    /* Simple approximation: use tip timestamp as mintime.
     * Full implementation would calculate actual MTP. */
    mintime = (uint32_t)(plat_time_ms() / 1000) - 600; /* Rough estimate */
  }

  json_builder_append(builder, ",\"mintime\":");
  json_builder_uint(builder, mintime);

  json_builder_append(builder, ",\"curtime\":");
  json_builder_uint(builder, (uint64_t)(plat_time_ms() / 1000));

  json_builder_append(builder, ",\"bits\":");
  char bits_hex[9];
  snprintf(bits_hex, sizeof(bits_hex), "%08x", bits);
  json_builder_string(builder, bits_hex);

  json_builder_append(builder, ",\"height\":");
  json_builder_uint(builder, next_height);

  /* Add sigoplimit and sizelimit for completeness */
  json_builder_append(builder, ",\"sigoplimit\":");
  json_builder_uint(builder, BLOCK_MAX_SIGOPS);

  json_builder_append(builder, ",\"weightlimit\":");
  json_builder_uint(builder, BLOCK_MAX_WEIGHT);

  json_builder_append(builder, "}");

  return ECHO_OK;
}

/* submitblock */
echo_result_t rpc_submitblock(node_t *node, const json_value_t *params,
                              json_builder_t *builder) {
  if (node == NULL || builder == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Get hex parameter */
  json_value_t *hex_val = json_array_get(params, 0);
  if (hex_val == NULL || hex_val->type != JSON_STRING) {
    return ECHO_ERR_INVALID_PARAM;
  }

  /* Decode hex */
  size_t hex_len = strlen(hex_val->u.string);
  size_t block_max_len = hex_len / 2;
  uint8_t *block_data = malloc(block_max_len);
  if (block_data == NULL) {
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  size_t block_len;
  echo_result_t res =
      rpc_hex_decode(hex_val->u.string, block_data, block_max_len, &block_len);
  if (res != ECHO_OK) {
    free(block_data);
    return res;
  }

  /* Parse block */
  block_t block;
  block_init(&block);
  res = block_parse(block_data, block_len, &block, NULL);
  free(block_data);

  if (res != ECHO_OK) {
    return res;
  }

  /* Validate block first */
  consensus_engine_t *consensus = node_get_consensus(node);
  consensus_result_t validation_result;
  consensus_result_init(&validation_result);

  bool valid = consensus_validate_block(consensus, &block, &validation_result);
  if (!valid) {
    block_free(&block);
    json_builder_string(builder, "invalid");
    return ECHO_OK;
  }

  /*
   * Apply block with persistence.
   * This updates consensus engine, block files, and databases atomically.
   */
  res = node_apply_block(node, &block);
  block_free(&block);

  if (res != ECHO_OK) {
    json_builder_string(builder, "rejected");
    return ECHO_OK;
  }

  /* Success - null means accepted */
  json_builder_null(builder);
  return ECHO_OK;
}

/*
 * ============================================================================
 * OBSERVER MODE RPC METHODS
 * ============================================================================
 */

/**
 * RPC: getobserverstats
 *
 * Returns observer mode statistics including message counts and peer count.
 *
 * Response:
 * {
 *   "mode": "observer",
 *   "uptime_seconds": 123,
 *   "peer_count": 5,
 *   "messages_received": {
 *     "version": 5,
 *     "verack": 5,
 *     "inv": 42,
 *     ...
 *   }
 * }
 */
static echo_result_t rpc_getobserverstats(node_t *node,
                                          const json_value_t *params,
                                          json_builder_t *builder) {
  (void)params; /* Unused */

  if (node == NULL || builder == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Observer stats are tracked in all modes */

  /* Get observer statistics */
  observer_stats_t stats;
  node_get_observer_stats(node, &stats);

  /* Get node statistics */
  node_stats_t node_stats;
  node_get_stats(node, &node_stats);

  /* Build JSON response */
  json_builder_append(builder, "{");

  /* Report actual mode */
  if (node_is_observer(node)) {
    json_builder_append(builder, "\"mode\":\"observer\",");
  } else {
    json_builder_append(builder, "\"mode\":\"full\",");
  }

  /* Uptime */
  uint64_t uptime_seconds = node_stats.uptime_ms / 1000;
  json_builder_append(builder, "\"uptime_seconds\":");
  json_builder_uint(builder, uptime_seconds);
  json_builder_append(builder, ",");

  /* Peer count */
  json_builder_append(builder, "\"peer_count\":");
  json_builder_uint(builder, (uint64_t)node_stats.peer_count);
  json_builder_append(builder, ",");

  /* Starting height (for "blocks this session" calculation) */
  json_builder_append(builder, "\"start_height\":");
  json_builder_uint(builder, (uint64_t)node_stats.start_height);
  json_builder_append(builder, ",");

  /* Message counts */
  json_builder_append(builder, "\"messages_received\":{");
  json_builder_append(builder, "\"version\":");
  json_builder_uint(builder, stats.msg_version);
  json_builder_append(builder, ",\"verack\":");
  json_builder_uint(builder, stats.msg_verack);
  json_builder_append(builder, ",\"ping\":");
  json_builder_uint(builder, stats.msg_ping);
  json_builder_append(builder, ",\"pong\":");
  json_builder_uint(builder, stats.msg_pong);
  json_builder_append(builder, ",\"addr\":");
  json_builder_uint(builder, stats.msg_addr);
  json_builder_append(builder, ",\"inv\":");
  json_builder_uint(builder, stats.msg_inv);
  json_builder_append(builder, ",\"getdata\":");
  json_builder_uint(builder, stats.msg_getdata);
  json_builder_append(builder, ",\"block\":");
  json_builder_uint(builder, stats.msg_block);
  json_builder_append(builder, ",\"tx\":");
  json_builder_uint(builder, stats.msg_tx);
  json_builder_append(builder, ",\"headers\":");
  json_builder_uint(builder, stats.msg_headers);
  json_builder_append(builder, ",\"getblocks\":");
  json_builder_uint(builder, stats.msg_getblocks);
  json_builder_append(builder, ",\"getheaders\":");
  json_builder_uint(builder, stats.msg_getheaders);
  json_builder_append(builder, ",\"other\":");
  json_builder_uint(builder, stats.msg_other);
  json_builder_append(builder, "}");

  json_builder_append(builder, "}");
  return ECHO_OK;
}

/**
 * RPC: getobservedblocks
 *
 * Returns recently observed block announcements.
 *
 * Response:
 * {
 *   "blocks": [
 *     {"hash": "00000000...", "first_seen": 1234567890, "peer_count": 3},
 *     ...
 *   ]
 * }
 */
static echo_result_t rpc_getobservedblocks(node_t *node,
                                           const json_value_t *params,
                                           json_builder_t *builder) {
  (void)params; /* Unused */

  if (node == NULL || builder == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Observer stats are tracked in all modes */

  /* Get observer statistics */
  observer_stats_t stats;
  node_get_observer_stats(node, &stats);

  /* Build JSON response */
  json_builder_append(builder, "{\"blocks\":[");

  /* Output blocks in chronological order (oldest first) */
  for (size_t i = 0; i < stats.block_count; i++) {
    const observer_block_t *block = &stats.blocks[i];

    if (i > 0) {
      json_builder_append(builder, ",");
    }

    json_builder_append(builder, "{");

    /* Block hash (reversed for display) */
    json_builder_append(builder, "\"hash\":\"");
    char hash_str[65];
    rpc_format_hash(&block->hash, hash_str);
    json_builder_append(builder, hash_str);
    json_builder_append(builder, "\",");

    /* First seen timestamp */
    json_builder_append(builder, "\"first_seen\":");
    json_builder_uint(builder, block->first_seen);
    json_builder_append(builder, ",");

    /* Peer count */
    json_builder_append(builder, "\"peer_count\":");
    json_builder_uint(builder, block->peer_count);

    json_builder_append(builder, "}");
  }

  json_builder_append(builder, "]}");
  return ECHO_OK;
}

/**
 * RPC: getobservedtxs
 *
 * Returns recently observed transaction announcements.
 *
 * Response:
 * {
 *   "transactions": [
 *     {"txid": "abc123...", "first_seen": 1234567890},
 *     ...
 *   ]
 * }
 */
static echo_result_t rpc_getobservedtxs(node_t *node,
                                        const json_value_t *params,
                                        json_builder_t *builder) {
  (void)params; /* Unused */

  if (node == NULL || builder == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Observer stats are tracked in all modes */

  /* Get observer statistics */
  observer_stats_t stats;
  node_get_observer_stats(node, &stats);

  /* Build JSON response */
  json_builder_append(builder, "{\"transactions\":[");

  /* Output transactions (most recent first, up to 100) */
  size_t count = stats.tx_count < 100 ? stats.tx_count : 100;
  for (size_t i = 0; i < count; i++) {
    /* Calculate index (most recent first) */
    size_t idx =
        (stats.tx_write_index + NODE_OBSERVER_MAX_TXS - 1 - i) % NODE_OBSERVER_MAX_TXS;
    const observer_tx_t *tx = &stats.txs[idx];

    if (i > 0) {
      json_builder_append(builder, ",");
    }

    json_builder_append(builder, "{");

    /* Transaction ID (reversed for display) */
    json_builder_append(builder, "\"txid\":\"");
    char txid_str[65];
    rpc_format_hash(&tx->txid, txid_str);
    json_builder_append(builder, txid_str);
    json_builder_append(builder, "\",");

    /* First seen timestamp */
    json_builder_append(builder, "\"first_seen\":");
    json_builder_uint(builder, tx->first_seen);

    json_builder_append(builder, "}");
  }

  json_builder_append(builder, "]}");
  return ECHO_OK;
}

/*
 * ============================================================================
 * PRUNING RPC METHOD
 * ============================================================================
 */

/**
 * RPC: pruneblockchain
 *
 * Manually prunes blocks up to the specified height.
 *
 * Parameters:
 *   height (number) - Target block height to prune up to
 *
 * Returns:
 *   The actual height pruned to (may be less than requested due to
 *   reorg safety margin).
 *
 * Errors:
 *   -1: Pruning is not enabled (--prune flag not set)
 *   -8: Invalid parameter
 */
static echo_result_t rpc_pruneblockchain(node_t *node, const json_value_t *params,
                                         json_builder_t *builder) {
  if (node == NULL || builder == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Check if pruning is enabled */
  if (!node_is_pruning_enabled(node)) {
    /* Return error: pruning not enabled */
    return RPC_ERR_MISC; /* "Cannot prune blocks when pruning is not enabled" */
  }

  /* Get height parameter */
  json_value_t *height_val = json_array_get(params, 0);
  if (height_val == NULL) {
    return ECHO_ERR_INVALID_PARAM;
  }

  uint32_t target_height = 0;
  if (height_val->type == JSON_NUMBER) {
    if (height_val->u.number < 0) {
      return ECHO_ERR_INVALID_PARAM;
    }
    target_height = (uint32_t)height_val->u.number;
  } else {
    return ECHO_ERR_INVALID_PARAM;
  }

  /* Perform pruning */
  uint32_t actual_height = node_prune_blocks(node, target_height);

  /* Return the actual height pruned to */
  json_builder_uint(builder, actual_height);
  return ECHO_OK;
}

/*
 * ============================================================================
 * SYNC STATUS RPC METHOD
 * ============================================================================
 */

/**
 * RPC: getsyncstatus
 *
 * Returns detailed sync metrics including rate and ETA.
 * This is the "source of truth" for sync progress that the GUI should display.
 *
 * Batch IBD phases: DOWNLOAD → DRAIN → VALIDATE → FLUSH → PRUNE (cycle)
 *
 * Response:
 * {
 *   "mode": "DOWNLOAD",
 *   "blocks_validated": 180000,
 *   "best_header_height": 875000,
 *   "tip_height": 180000,
 *   "blocks_pending": 695000,
 *   "blocks_in_flight": 512,
 *   "sync_percentage": 20.57,
 *   "download_rate_bps": 120.5,
 *   "validation_rate_bps": 48.5,
 *   "eta_seconds": 14320,
 *   "storage_used_bytes": 536870912,
 *   "storage_prune_target": 1073741824,
 *   "active_sync_peers": 6,
 *   "total_peers": 8,
 *   "initialblockdownload": true
 * }
 */
static echo_result_t rpc_getsyncstatus(node_t *node, const json_value_t *params,
                                       json_builder_t *builder) {
  (void)params; /* Unused */

  if (node == NULL || builder == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Get sync manager and progress */
  sync_manager_t *sync_mgr = node_get_sync_manager(node);
  if (sync_mgr == NULL) {
    /* No sync manager - return minimal response */
    json_builder_append(builder, "{\"mode\":\"idle\",\"initialblockdownload\":false}");
    return ECHO_OK;
  }

  /* Get progress and metrics */
  sync_progress_t progress = {0};
  sync_get_progress(sync_mgr, &progress);

  sync_metrics_t metrics = {0};
  sync_get_metrics(sync_mgr, &metrics);

  /* Get node stats for peer count */
  node_stats_t node_stats;
  node_get_stats(node, &node_stats);

  /* Build JSON response */
  json_builder_append(builder, "{");

  /* Sync mode */
  json_builder_append(builder, "\"mode\":");
  json_builder_string(builder, metrics.mode_string);

  /* Block heights */
  json_builder_append(builder, ",\"blocks_validated\":");
  json_builder_uint(builder, progress.blocks_validated);

  json_builder_append(builder, ",\"best_header_height\":");
  json_builder_uint(builder, progress.best_header_height);

  json_builder_append(builder, ",\"tip_height\":");
  json_builder_uint(builder, progress.tip_height);

  json_builder_append(builder, ",\"blocks_downloaded\":");
  json_builder_uint(builder, progress.blocks_downloaded);

  json_builder_append(builder, ",\"blocks_pending\":");
  json_builder_uint(builder, progress.blocks_pending);

  json_builder_append(builder, ",\"blocks_in_flight\":");
  json_builder_uint(builder, (uint64_t)progress.blocks_in_flight);

  /* Sync percentage */
  json_builder_append(builder, ",\"sync_percentage\":");
  json_builder_number(builder, progress.sync_percentage);

  /* Performance metrics (source of truth for GUI) */
  json_builder_append(builder, ",\"download_rate_bps\":");
  json_builder_number(builder, metrics.download_rate_bps);

  json_builder_append(builder, ",\"validation_rate_bps\":");
  json_builder_number(builder, metrics.validation_rate_bps);

  json_builder_append(builder, ",\"eta_seconds\":");
  json_builder_uint(builder, metrics.eta_seconds);

  /* Storage metrics for batch IBD */
  json_builder_append(builder, ",\"storage_used_bytes\":");
  json_builder_uint(builder, metrics.storage_used_bytes);

  json_builder_append(builder, ",\"storage_prune_target\":");
  json_builder_uint(builder, metrics.storage_prune_target);

  /* Peer info */
  json_builder_append(builder, ",\"active_sync_peers\":");
  json_builder_uint(builder, metrics.active_sync_peers);

  json_builder_append(builder, ",\"total_peers\":");
  json_builder_uint(builder, (uint64_t)node_stats.peer_count);

  /* IBD flag */
  json_builder_append(builder, ",\"initialblockdownload\":");
  json_builder_bool(builder, node_stats.is_syncing);

  json_builder_append(builder, "}");

  return ECHO_OK;
}
