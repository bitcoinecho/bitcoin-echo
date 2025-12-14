/**
 * Bitcoin Echo — RPC Interface Tests
 *
 * Tests for Session 9.3: RPC Interface
 *
 * Verifies:
 * - JSON parsing (embedded minimal parser)
 * - JSON building (response construction)
 * - RPC request/response formatting
 * - Utility functions (hex encoding, hash formatting)
 * - RPC method handlers (unit tests with mock node)
 *
 * Build once. Build right. Stop.
 */

#include "rpc.h"
#include "block.h"
#include "consensus.h"
#include "echo_types.h"
#include "mempool.h"
#include "node.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_utils.h"

/* Test counter */

#define PASS()                                                                 \
  do {                                                                         \
    printf(" PASS\n");                                                         \
    tests_passed++;                                                            \
  } while (0)

/*
 * ============================================================================
 * TEST: JSON Parser - Basic Types
 * ============================================================================
 */

static void test_json_parse_null(void) {
  json_value_t *value = NULL;
  echo_result_t res = json_parse("null", &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_NULL);

  json_free(value);
  test_pass();
}

static void test_json_parse_true(void) {
  json_value_t *value = NULL;
  echo_result_t res = json_parse("true", &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_BOOL);
  assert(value->u.boolean == true);

  json_free(value);
  test_pass();
}

static void test_json_parse_false(void) {
  json_value_t *value = NULL;
  echo_result_t res = json_parse("false", &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_BOOL);
  assert(value->u.boolean == false);

  json_free(value);
  test_pass();
}

static void test_json_parse_integer(void) {
  json_value_t *value = NULL;
  echo_result_t res = json_parse("42", &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_NUMBER);
  assert(value->u.number == 42.0);

  json_free(value);
  test_pass();
}

static void test_json_parse_negative(void) {
  json_value_t *value = NULL;
  echo_result_t res = json_parse("-123", &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_NUMBER);
  assert(value->u.number == -123.0);

  json_free(value);
  test_pass();
}

static void test_json_parse_float(void) {
  json_value_t *value = NULL;
  echo_result_t res = json_parse("3.14159", &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_NUMBER);
  assert(value->u.number > 3.14 && value->u.number < 3.15);

  json_free(value);
  test_pass();
}

static void test_json_parse_string(void) {
  json_value_t *value = NULL;
  echo_result_t res = json_parse("\"hello world\"", &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_STRING);
  assert(strcmp(value->u.string, "hello world") == 0);

  json_free(value);
  test_pass();
}

static void test_json_parse_escaped_string(void) {
  json_value_t *value = NULL;
  echo_result_t res = json_parse("\"line1\\nline2\"", &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_STRING);
  assert(strcmp(value->u.string, "line1\nline2") == 0);

  json_free(value);
  test_pass();
}

static void test_json_parse_empty_string(void) {
  json_value_t *value = NULL;
  echo_result_t res = json_parse("\"\"", &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_STRING);
  assert(strlen(value->u.string) == 0);

  json_free(value);
  test_pass();
}

/*
 * ============================================================================
 * TEST: JSON Parser - Arrays
 * ============================================================================
 */

static void test_json_parse_empty_array(void) {
  json_value_t *value = NULL;
  echo_result_t res = json_parse("[]", &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_ARRAY);
  assert(json_array_length(value) == 0);

  json_free(value);
  test_pass();
}

static void test_json_parse_array_numbers(void) {
  json_value_t *value = NULL;
  echo_result_t res = json_parse("[1, 2, 3]", &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_ARRAY);
  assert(json_array_length(value) == 3);

  json_value_t *elem0 = json_array_get(value, 0);
  json_value_t *elem1 = json_array_get(value, 1);
  json_value_t *elem2 = json_array_get(value, 2);

  assert(elem0 != NULL && elem0->type == JSON_NUMBER && elem0->u.number == 1.0);
  assert(elem1 != NULL && elem1->type == JSON_NUMBER && elem1->u.number == 2.0);
  assert(elem2 != NULL && elem2->type == JSON_NUMBER && elem2->u.number == 3.0);

  json_free(value);
  test_pass();
}

static void test_json_parse_mixed_array(void) {
  json_value_t *value = NULL;
  echo_result_t res = json_parse("[1, \"two\", true, null]", &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_ARRAY);
  assert(json_array_length(value) == 4);

  assert(json_array_get(value, 0)->type == JSON_NUMBER);
  assert(json_array_get(value, 1)->type == JSON_STRING);
  assert(json_array_get(value, 2)->type == JSON_BOOL);
  assert(json_array_get(value, 3)->type == JSON_NULL);

  json_free(value);
  test_pass();
}

/*
 * ============================================================================
 * TEST: JSON Parser - Objects
 * ============================================================================
 */

static void test_json_parse_empty_object(void) {
  json_value_t *value = NULL;
  echo_result_t res = json_parse("{}", &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_OBJECT);

  json_free(value);
  test_pass();
}

static void test_json_parse_simple_object(void) {
  json_value_t *value = NULL;
  echo_result_t res = json_parse("{\"name\": \"echo\", \"version\": 1}", &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_OBJECT);

  json_value_t *name = json_object_get(value, "name");
  json_value_t *version = json_object_get(value, "version");

  assert(name != NULL && name->type == JSON_STRING);
  assert(strcmp(name->u.string, "echo") == 0);
  assert(version != NULL && version->type == JSON_NUMBER);
  assert(version->u.number == 1.0);

  json_free(value);
  test_pass();
}

static void test_json_parse_nested_object(void) {
  json_value_t *value = NULL;
  echo_result_t res =
      json_parse("{\"outer\": {\"inner\": 42}}", &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_OBJECT);

  json_value_t *outer = json_object_get(value, "outer");
  assert(outer != NULL && outer->type == JSON_OBJECT);

  json_value_t *inner = json_object_get(outer, "inner");
  assert(inner != NULL && inner->type == JSON_NUMBER);
  assert(inner->u.number == 42.0);

  json_free(value);
  test_pass();
}

/*
 * ============================================================================
 * TEST: JSON Parser - RPC Format
 * ============================================================================
 */

static void test_json_parse_rpc_request(void) {
  const char *json =
      "{\"id\": 1, \"method\": \"getblockchaininfo\", \"params\": []}";

  json_value_t *value = NULL;
  echo_result_t res = json_parse(json, &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_OBJECT);

  json_value_t *id = json_object_get(value, "id");
  json_value_t *method = json_object_get(value, "method");
  json_value_t *params = json_object_get(value, "params");

  assert(id != NULL && id->type == JSON_NUMBER);
  assert(method != NULL && method->type == JSON_STRING);
  assert(params != NULL && params->type == JSON_ARRAY);
  assert(strcmp(method->u.string, "getblockchaininfo") == 0);

  json_free(value);
  test_pass();
}

static void test_json_parse_rpc_with_params(void) {
  const char *json =
      "{\"id\": \"test\", \"method\": \"getblock\", \"params\": "
      "[\"000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f\", 1]}";

  json_value_t *value = NULL;
  echo_result_t res = json_parse(json, &value);

  assert(res == ECHO_OK);
  assert(value != NULL);

  json_value_t *params = json_object_get(value, "params");
  assert(params != NULL && params->type == JSON_ARRAY);
  assert(json_array_length(params) == 2);

  json_value_t *hash = json_array_get(params, 0);
  json_value_t *verbosity = json_array_get(params, 1);

  assert(hash != NULL && hash->type == JSON_STRING);
  assert(strlen(hash->u.string) == 64); /* 64 hex chars */
  assert(verbosity != NULL && verbosity->type == JSON_NUMBER);
  assert(verbosity->u.number == 1.0);

  json_free(value);
  test_pass();
}

/*
 * ============================================================================
 * TEST: JSON Builder
 * ============================================================================
 */

static void test_json_builder_string(void) {
  json_builder_t builder;
  json_builder_init(&builder);

  json_builder_string(&builder, "hello");
  assert(strcmp(json_builder_str(&builder), "\"hello\"") == 0);

  json_builder_free(&builder);
  test_pass();
}

static void test_json_builder_number(void) {
  json_builder_t builder;
  json_builder_init(&builder);

  json_builder_int(&builder, 42);
  assert(strcmp(json_builder_str(&builder), "42") == 0);

  json_builder_free(&builder);
  test_pass();
}

static void test_json_builder_bool(void) {
  json_builder_t builder;
  json_builder_init(&builder);

  json_builder_bool(&builder, true);
  assert(strcmp(json_builder_str(&builder), "true") == 0);

  json_builder_free(&builder);
  test_pass();
}

static void test_json_builder_null(void) {
  json_builder_t builder;
  json_builder_init(&builder);

  json_builder_null(&builder);
  assert(strcmp(json_builder_str(&builder), "null") == 0);

  json_builder_free(&builder);
  test_pass();
}

static void test_json_builder_hex(void) {
  uint8_t data[] = {0xde, 0xad, 0xbe, 0xef};
  json_builder_t builder;
  json_builder_init(&builder);

  json_builder_hex(&builder, data, 4);
  assert(strcmp(json_builder_str(&builder), "\"deadbeef\"") == 0);

  json_builder_free(&builder);
  test_pass();
}

static void test_json_builder_object(void) {
  json_builder_t builder;
  json_builder_init(&builder);

  json_builder_append(&builder, "{");
  json_builder_append(&builder, "\"key\":");
  json_builder_string(&builder, "value");
  json_builder_append(&builder, "}");

  assert(strcmp(json_builder_str(&builder), "{\"key\":\"value\"}") == 0);

  json_builder_free(&builder);
  test_pass();
}

/*
 * ============================================================================
 * TEST: RPC Request Parsing
 * ============================================================================
 */

static void test_rpc_request_parse_basic(void) {
  const char *json =
      "{\"id\": \"1\", \"method\": \"getblockchaininfo\", \"params\": []}";

  rpc_request_t req;
  echo_result_t res = rpc_request_parse(json, &req);

  assert(res == ECHO_OK);
  assert(req.id != NULL);
  assert(strcmp(req.id, "1") == 0);
  assert(req.method != NULL);
  assert(strcmp(req.method, "getblockchaininfo") == 0);
  assert(req.params != NULL);

  rpc_request_free(&req);
  test_pass();
}

static void test_rpc_request_parse_numeric_id(void) {
  const char *json =
      "{\"id\": 42, \"method\": \"getblock\", \"params\": [\"hash\"]}";

  rpc_request_t req;
  echo_result_t res = rpc_request_parse(json, &req);

  assert(res == ECHO_OK);
  assert(req.id != NULL);
  assert(strcmp(req.id, "42") == 0);
  assert(strcmp(req.method, "getblock") == 0);

  rpc_request_free(&req);
  test_pass();
}

static void test_rpc_request_parse_null_id(void) {
  const char *json = "{\"id\": null, \"method\": \"test\", \"params\": []}";

  rpc_request_t req;
  echo_result_t res = rpc_request_parse(json, &req);

  assert(res == ECHO_OK);
  assert(req.id == NULL);
  assert(strcmp(req.method, "test") == 0);

  rpc_request_free(&req);
  test_pass();
}

/*
 * ============================================================================
 * TEST: RPC Response Building
 * ============================================================================
 */

static void test_rpc_response_success(void) {
  json_builder_t builder;
  echo_result_t res = rpc_response_success("1", "42", &builder);

  assert(res == ECHO_OK);

  const char *response = json_builder_str(&builder);
  assert(strstr(response, "\"result\":42") != NULL);
  assert(strstr(response, "\"error\":null") != NULL);
  assert(strstr(response, "\"id\":\"1\"") != NULL);

  json_builder_free(&builder);
  test_pass();
}

static void test_rpc_response_error(void) {
  json_builder_t builder;
  echo_result_t res =
      rpc_response_error("1", RPC_ERR_METHOD_NOT_FOUND, "Method not found", &builder);

  assert(res == ECHO_OK);

  const char *response = json_builder_str(&builder);
  assert(strstr(response, "\"result\":null") != NULL);
  assert(strstr(response, "\"error\":{") != NULL);
  assert(strstr(response, "\"code\":-32601") != NULL);
  assert(strstr(response, "\"message\":\"Method not found\"") != NULL);

  json_builder_free(&builder);
  test_pass();
}

/*
 * ============================================================================
 * TEST: Utility Functions
 * ============================================================================
 */

static void test_hex_decode_valid(void) {
  uint8_t out[4];
  size_t written;
  echo_result_t res = rpc_hex_decode("deadbeef", out, sizeof(out), &written);

  assert(res == ECHO_OK);
  assert(written == 4);
  assert(out[0] == 0xde);
  assert(out[1] == 0xad);
  assert(out[2] == 0xbe);
  assert(out[3] == 0xef);

  test_pass();
}

static void test_hex_decode_uppercase(void) {
  uint8_t out[4];
  size_t written;
  echo_result_t res = rpc_hex_decode("DEADBEEF", out, sizeof(out), &written);

  assert(res == ECHO_OK);
  assert(written == 4);
  assert(out[0] == 0xde);
  assert(out[1] == 0xad);

  test_pass();
}

static void test_hex_decode_odd_length(void) {
  uint8_t out[4];
  size_t written;
  echo_result_t res = rpc_hex_decode("abc", out, sizeof(out), &written);

  assert(res == ECHO_ERR_INVALID_FORMAT);

  test_pass();
}

static void test_format_hash(void) {

  hash256_t hash = {0};
  /* Genesis block hash in internal byte order */
  hash.bytes[0] = 0x6f;
  hash.bytes[1] = 0xe2;
  hash.bytes[31] = 0x00;

  char out[65];
  rpc_format_hash(&hash, out);

  /* Should be reversed for display */
  assert(strlen(out) == 64);
  assert(out[0] == '0' && out[1] == '0'); /* High byte first in display */

  test_pass();
}

static void test_parse_hash(void) {
  /* Genesis block hash in display format */
  const char *hex =
      "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f";

  hash256_t hash;
  echo_result_t res = rpc_parse_hash(hex, &hash);

  assert(res == ECHO_OK);
  /* Check internal byte order (reversed from display) */
  assert(hash.bytes[31] == 0x00);
  assert(hash.bytes[0] == 0x6f);

  test_pass();
}

static void test_parse_hash_invalid_length(void) {
  hash256_t hash;
  echo_result_t res = rpc_parse_hash("0123456789", &hash);

  assert(res == ECHO_ERR_INVALID_FORMAT);

  test_pass();
}

/*
 * ============================================================================
 * TEST: RPC Server Lifecycle
 * ============================================================================
 */

static void test_rpc_config_init(void) {
  rpc_config_t config;
  rpc_config_init(&config);

  assert(config.port == RPC_DEFAULT_PORT);
  assert(config.bind_addr == NULL);

  test_pass();
}

static void test_rpc_server_create_null_node(void) {
  rpc_config_t config;
  rpc_config_init(&config);

  rpc_server_t *server = rpc_server_create(&config, NULL);
  assert(server == NULL);

  test_pass();
}

/*
 * ============================================================================
 * TEST: JSON Parser - Error Cases
 * ============================================================================
 */

static void test_json_parse_invalid_json(void) {
  json_value_t *value = NULL;
  echo_result_t res = json_parse("{invalid", &value);

  assert(res == ECHO_ERR_INVALID_FORMAT);
  assert(value == NULL);

  test_pass();
}

static void test_json_parse_null_input(void) {
  json_value_t *value = NULL;
  echo_result_t res = json_parse(NULL, &value);

  assert(res == ECHO_ERR_NULL_PARAM);

  test_pass();
}

static void test_json_parse_null_output(void) {
  echo_result_t res = json_parse("{}", NULL);

  assert(res == ECHO_ERR_NULL_PARAM);

  test_pass();
}

/*
 * ============================================================================
 * TEST: Batch Request Parsing (Session 9.5+)
 * ============================================================================
 */

static void test_json_parse_batch_request(void) {
  const char *json =
      "[{\"jsonrpc\":\"2.0\",\"method\":\"getobserverstats\",\"params\":[],"
      "\"id\":1},"
      "{\"jsonrpc\":\"2.0\",\"method\":\"getobservedblocks\",\"params\":[],"
      "\"id\":2}]";

  json_value_t *value = NULL;
  echo_result_t res = json_parse(json, &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_ARRAY);
  assert(json_array_length(value) == 2);

  /* First request */
  json_value_t *req1 = json_array_get(value, 0);
  assert(req1 != NULL && req1->type == JSON_OBJECT);
  json_value_t *method1 = json_object_get(req1, "method");
  assert(method1 != NULL && method1->type == JSON_STRING);
  assert(strcmp(method1->u.string, "getobserverstats") == 0);

  /* Second request */
  json_value_t *req2 = json_array_get(value, 1);
  assert(req2 != NULL && req2->type == JSON_OBJECT);
  json_value_t *method2 = json_object_get(req2, "method");
  assert(method2 != NULL && method2->type == JSON_STRING);
  assert(strcmp(method2->u.string, "getobservedblocks") == 0);

  json_free(value);
  test_pass();
}

static void test_json_parse_empty_batch(void) {
  const char *json = "[]";

  json_value_t *value = NULL;
  echo_result_t res = json_parse(json, &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_ARRAY);
  assert(json_array_length(value) == 0);

  json_free(value);
  test_pass();
}

static void test_json_parse_batch_mixed_ids(void) {
  const char *json =
      "[{\"method\":\"test1\",\"id\":1},"
      "{\"method\":\"test2\",\"id\":\"string\"},"
      "{\"method\":\"test3\",\"id\":null}]";

  json_value_t *value = NULL;
  echo_result_t res = json_parse(json, &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_ARRAY);
  assert(json_array_length(value) == 3);

  /* Check first ID (number) */
  json_value_t *req1 = json_array_get(value, 0);
  json_value_t *id1 = json_object_get(req1, "id");
  assert(id1 != NULL && id1->type == JSON_NUMBER);
  assert(id1->u.number == 1.0);

  /* Check second ID (string) */
  json_value_t *req2 = json_array_get(value, 1);
  json_value_t *id2 = json_object_get(req2, "id");
  assert(id2 != NULL && id2->type == JSON_STRING);
  assert(strcmp(id2->u.string, "string") == 0);

  /* Check third ID (null) */
  json_value_t *req3 = json_array_get(value, 2);
  json_value_t *id3 = json_object_get(req3, "id");
  assert(id3 != NULL && id3->type == JSON_NULL);

  json_free(value);
  test_pass();
}

static void test_json_builder_batch_response(void) {
  json_builder_t builder;
  json_builder_init(&builder);

  /* Build a batch response array */
  json_builder_append(&builder, "[");

  /* First response */
  json_builder_append(&builder, "{\"result\":");
  json_builder_int(&builder, 42);
  json_builder_append(&builder, ",\"error\":null,\"id\":1}");

  json_builder_append(&builder, ",");

  /* Second response */
  json_builder_append(&builder, "{\"result\":");
  json_builder_string(&builder, "success");
  json_builder_append(&builder, ",\"error\":null,\"id\":2}");

  json_builder_append(&builder, "]");

  const char *result = json_builder_str(&builder);
  assert(strstr(result, "[{") != NULL);
  assert(strstr(result, "\"result\":42") != NULL);
  assert(strstr(result, "\"result\":\"success\"") != NULL);
  assert(strstr(result, "}]") != NULL);

  json_builder_free(&builder);
  test_pass();
}

static void test_json_parse_batch_with_invalid_request(void) {
  /* Batch with one valid and one invalid request */
  const char *json =
      "[{\"method\":\"test\",\"id\":1},"
      "\"invalid_request\"]";

  json_value_t *value = NULL;
  echo_result_t res = json_parse(json, &value);

  assert(res == ECHO_OK);
  assert(value != NULL);
  assert(value->type == JSON_ARRAY);
  assert(json_array_length(value) == 2);

  /* First should be object */
  json_value_t *req1 = json_array_get(value, 0);
  assert(req1 != NULL && req1->type == JSON_OBJECT);

  /* Second should be string (invalid request object) */
  json_value_t *req2 = json_array_get(value, 1);
  assert(req2 != NULL && req2->type == JSON_STRING);

  json_free(value);
  test_pass();
}

/*
 * ============================================================================
 * Main
 * ============================================================================
 */

int main(void) {
    test_suite_begin("Rpc Tests");

    test_case("Json parse null"); test_json_parse_null(); test_pass();
    test_case("Json parse true"); test_json_parse_true(); test_pass();
    test_case("Json parse false"); test_json_parse_false(); test_pass();
    test_case("Json parse integer"); test_json_parse_integer(); test_pass();
    test_case("Json parse negative"); test_json_parse_negative(); test_pass();
    test_case("Json parse float"); test_json_parse_float(); test_pass();
    test_case("Json parse string"); test_json_parse_string(); test_pass();
    test_case("Json parse escaped string"); test_json_parse_escaped_string(); test_pass();
    test_case("Json parse empty string"); test_json_parse_empty_string(); test_pass();
    test_case("Json parse empty array"); test_json_parse_empty_array(); test_pass();
    test_case("Json parse array numbers"); test_json_parse_array_numbers(); test_pass();
    test_case("Json parse mixed array"); test_json_parse_mixed_array(); test_pass();
    test_case("Json parse empty object"); test_json_parse_empty_object(); test_pass();
    test_case("Json parse simple object"); test_json_parse_simple_object(); test_pass();
    test_case("Json parse nested object"); test_json_parse_nested_object(); test_pass();
    test_case("Json parse rpc request"); test_json_parse_rpc_request(); test_pass();
    test_case("Json parse rpc with params"); test_json_parse_rpc_with_params(); test_pass();
    test_case("Json builder string"); test_json_builder_string(); test_pass();
    test_case("Json builder number"); test_json_builder_number(); test_pass();
    test_case("Json builder bool"); test_json_builder_bool(); test_pass();
    test_case("Json builder null"); test_json_builder_null(); test_pass();
    test_case("Json builder hex"); test_json_builder_hex(); test_pass();
    test_case("Json builder object"); test_json_builder_object(); test_pass();
    test_case("Rpc request parse basic"); test_rpc_request_parse_basic(); test_pass();
    test_case("Rpc request parse numeric id"); test_rpc_request_parse_numeric_id(); test_pass();
    test_case("Rpc request parse null id"); test_rpc_request_parse_null_id(); test_pass();
    test_case("Rpc response success"); test_rpc_response_success(); test_pass();
    test_case("Rpc response error"); test_rpc_response_error(); test_pass();
    test_case("Hex decode valid"); test_hex_decode_valid(); test_pass();
    test_case("Hex decode uppercase"); test_hex_decode_uppercase(); test_pass();
    test_case("Hex decode odd length"); test_hex_decode_odd_length(); test_pass();
    test_case("Format hash"); test_format_hash(); test_pass();
    test_case("Parse hash"); test_parse_hash(); test_pass();
    test_case("Parse hash invalid length"); test_parse_hash_invalid_length(); test_pass();
    test_case("Rpc config init"); test_rpc_config_init(); test_pass();
    test_case("Rpc server create null node"); test_rpc_server_create_null_node(); test_pass();
    test_case("Json parse invalid json"); test_json_parse_invalid_json(); test_pass();
    test_case("Json parse null input"); test_json_parse_null_input(); test_pass();
    test_case("Json parse null output"); test_json_parse_null_output(); test_pass();
    test_case("Json parse batch request"); test_json_parse_batch_request(); test_pass();
    test_case("Json parse empty batch"); test_json_parse_empty_batch(); test_pass();
    test_case("Json parse batch mixed ids"); test_json_parse_batch_mixed_ids(); test_pass();
    test_case("Json builder batch response"); test_json_builder_batch_response(); test_pass();
    test_case("Json parse batch with invalid request"); test_json_parse_batch_with_invalid_request(); test_pass();

    test_suite_end();
    return test_global_summary();
}
