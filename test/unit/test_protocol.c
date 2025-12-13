/**
 * Unit tests for protocol message structures
 */

#include "protocol.h"
#include "sha256.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "test_utils.h"


/* Test command parsing */
static void test_msg_parse_command_valid(void) {
    /* Test all valid commands */
    assert(msg_parse_command("version") == MSG_VERSION);
    assert(msg_parse_command("verack") == MSG_VERACK);
    assert(msg_parse_command("ping") == MSG_PING);
    assert(msg_parse_command("pong") == MSG_PONG);
    assert(msg_parse_command("inv") == MSG_INV);
    assert(msg_parse_command("getdata") == MSG_GETDATA);
    assert(msg_parse_command("notfound") == MSG_NOTFOUND);
    assert(msg_parse_command("block") == MSG_BLOCK);
    assert(msg_parse_command("tx") == MSG_TX);
    assert(msg_parse_command("addr") == MSG_ADDR);
    assert(msg_parse_command("getaddr") == MSG_GETADDR);
    assert(msg_parse_command("getheaders") == MSG_GETHEADERS);
    assert(msg_parse_command("getblocks") == MSG_GETBLOCKS);
    assert(msg_parse_command("headers") == MSG_HEADERS);
    assert(msg_parse_command("reject") == MSG_REJECT);
    assert(msg_parse_command("sendheaders") == MSG_SENDHEADERS);
    assert(msg_parse_command("feefilter") == MSG_FEEFILTER);
    assert(msg_parse_command("sendcmpct") == MSG_SENDCMPCT);
    assert(msg_parse_command("wtxidrelay") == MSG_WTXIDRELAY);
}

static void test_msg_parse_command_invalid(void) {
    /* Test unknown commands */
    assert(msg_parse_command("unknown") == MSG_UNKNOWN);
    assert(msg_parse_command("xyz") == MSG_UNKNOWN);
    assert(msg_parse_command("") == MSG_UNKNOWN);
}

static void test_msg_parse_command_no_null(void) {
    /* Command without null terminator should be rejected */
    char no_null[COMMAND_LEN];
    memset(no_null, 'x', COMMAND_LEN);
    assert(msg_parse_command(no_null) == MSG_UNKNOWN);
}

static void test_msg_parse_command_padded(void) {
    /* Command with null padding (standard wire format) */
    char padded[COMMAND_LEN];
    memset(padded, 0, COMMAND_LEN);
    strcpy(padded, "version");
    assert(msg_parse_command(padded) == MSG_VERSION);
}

/* Test command string retrieval */
static void test_msg_command_string(void) {
    assert(strcmp(msg_command_string(MSG_VERSION), "version") == 0);
    assert(strcmp(msg_command_string(MSG_VERACK), "verack") == 0);
    assert(strcmp(msg_command_string(MSG_PING), "ping") == 0);
    assert(strcmp(msg_command_string(MSG_PONG), "pong") == 0);
    assert(strcmp(msg_command_string(MSG_INV), "inv") == 0);
    assert(strcmp(msg_command_string(MSG_GETDATA), "getdata") == 0);
    assert(strcmp(msg_command_string(MSG_NOTFOUND), "notfound") == 0);
    assert(strcmp(msg_command_string(MSG_BLOCK), "block") == 0);
    assert(strcmp(msg_command_string(MSG_TX), "tx") == 0);
    assert(strcmp(msg_command_string(MSG_ADDR), "addr") == 0);
    assert(strcmp(msg_command_string(MSG_GETADDR), "getaddr") == 0);
    assert(strcmp(msg_command_string(MSG_GETHEADERS), "getheaders") == 0);
    assert(strcmp(msg_command_string(MSG_GETBLOCKS), "getblocks") == 0);
    assert(strcmp(msg_command_string(MSG_HEADERS), "headers") == 0);
    assert(strcmp(msg_command_string(MSG_REJECT), "reject") == 0);
    assert(strcmp(msg_command_string(MSG_SENDHEADERS), "sendheaders") == 0);
    assert(strcmp(msg_command_string(MSG_FEEFILTER), "feefilter") == 0);
    assert(strcmp(msg_command_string(MSG_SENDCMPCT), "sendcmpct") == 0);
    assert(strcmp(msg_command_string(MSG_WTXIDRELAY), "wtxidrelay") == 0);
}

static void test_msg_command_string_unknown(void) {
    /* Unknown type should return NULL */
    assert(msg_command_string(MSG_UNKNOWN) == NULL);
    assert(msg_command_string((msg_type_t)999) == NULL);
}

/* Test checksum computation */
static void test_msg_checksum_empty(void) {
    /* Empty payload */
    uint32_t checksum = msg_checksum(NULL, 0);

    /* SHA256d of empty string:
     * SHA256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
     * SHA256(e3b0c442...) = 5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456
     * First 4 bytes (LE): 0xe2e0f65d
     */
    assert(checksum == 0xe2e0f65d);
}

static void test_msg_checksum_known(void) {
    /* Known payload: "hello" */
    uint8_t payload[] = "hello";
    uint32_t checksum = msg_checksum(payload, 5);

    /* SHA256d("hello"):
     * SHA256("hello") = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
     * SHA256(2cf24dba...) = 9595c9df90075148eb06860365df33584b75bff782a510c6cd4883a419833d50
     * First 4 bytes (LE): 0xdfc99595
     */
    assert(checksum == 0xdfc99595);
}

static void test_msg_checksum_verack(void) {
    /* Verack message has empty payload, same as empty test */
    uint32_t checksum = msg_checksum(NULL, 0);
    assert(checksum == 0xe2e0f65d);
}

/* Test header validation */
static void test_msg_header_valid_mainnet(void) {
    msg_header_t header;
    header.magic = MAGIC_MAINNET;
    strcpy(header.command, "version");
    header.length = 100;
    header.checksum = 0x12345678;

    assert(msg_header_valid(&header, MAGIC_MAINNET) == ECHO_TRUE);
}

static void test_msg_header_valid_testnet(void) {
    msg_header_t header;
    header.magic = MAGIC_TESTNET;
    strcpy(header.command, "ping");
    header.length = 8;
    header.checksum = 0x12345678;

    assert(msg_header_valid(&header, MAGIC_TESTNET) == ECHO_TRUE);
}

static void test_msg_header_valid_regtest(void) {
    msg_header_t header;
    header.magic = MAGIC_REGTEST;
    strcpy(header.command, "pong");
    header.length = 8;
    header.checksum = 0x12345678;

    assert(msg_header_valid(&header, MAGIC_REGTEST) == ECHO_TRUE);
}

static void test_msg_header_invalid_magic(void) {
    msg_header_t header;
    header.magic = 0xDEADBEEF;  /* Wrong magic */
    strcpy(header.command, "version");
    header.length = 100;
    header.checksum = 0x12345678;

    assert(msg_header_valid(&header, MAGIC_MAINNET) == ECHO_FALSE);
}

static void test_msg_header_no_null_terminator(void) {
    msg_header_t header;
    header.magic = MAGIC_MAINNET;
    memset(header.command, 'x', COMMAND_LEN);  /* No null terminator */
    header.length = 100;
    header.checksum = 0x12345678;

    assert(msg_header_valid(&header, MAGIC_MAINNET) == ECHO_FALSE);
}

static void test_msg_header_oversized_payload(void) {
    msg_header_t header;
    header.magic = MAGIC_MAINNET;
    strcpy(header.command, "block");
    header.length = MAX_MESSAGE_SIZE + 1;  /* Too large */
    header.checksum = 0x12345678;

    assert(msg_header_valid(&header, MAGIC_MAINNET) == ECHO_FALSE);
}

static void test_msg_header_max_size_payload(void) {
    msg_header_t header;
    header.magic = MAGIC_MAINNET;
    strcpy(header.command, "block");
    header.length = MAX_MESSAGE_SIZE;  /* Exactly at limit */
    header.checksum = 0x12345678;

    assert(msg_header_valid(&header, MAGIC_MAINNET) == ECHO_TRUE);
}

static void test_msg_header_zero_length(void) {
    msg_header_t header;
    header.magic = MAGIC_MAINNET;
    strcpy(header.command, "verack");
    header.length = 0;  /* Empty payload */
    header.checksum = 0xe2e0f65d;

    assert(msg_header_valid(&header, MAGIC_MAINNET) == ECHO_TRUE);
}

/* Test message header size */
static void test_msg_header_size(void) {
    /* Header should be exactly 24 bytes */
    assert(sizeof(msg_header_t) == 24);
}

/* Test command padding */
static void test_command_padding(void) {
    msg_header_t header;
    header.magic = MAGIC_MAINNET;

    /* Clear command buffer */
    memset(header.command, 0, COMMAND_LEN);

    /* Set short command */
    strcpy(header.command, "ping");

    /* Remaining bytes should be zero (null padding) */
    for (size_t i = 5; i < COMMAND_LEN; i++) {
        assert(header.command[i] == 0);
    }

    header.length = 8;
    header.checksum = 0x12345678;

    assert(msg_header_valid(&header, MAGIC_MAINNET) == ECHO_TRUE);
}

/* Test round-trip command conversion */
static void test_command_roundtrip(void) {
    for (msg_type_t type = MSG_VERSION; type < MSG_UNKNOWN; type++) {
        const char *cmd = msg_command_string(type);
        assert(cmd != NULL);

        msg_type_t parsed = msg_parse_command(cmd);
        assert(parsed == type);
    }
}

/* Test inventory type constants */
static void test_inv_types(void) {
    /* Verify standard inventory types */
    assert(INV_ERROR == 0);
    assert(INV_TX == 1);
    assert(INV_BLOCK == 2);
    assert(INV_FILTERED_BLOCK == 3);

    /* Witness types have MSG_WITNESS_FLAG set */
    assert(INV_WITNESS_TX == 0x40000001);
    assert(INV_WITNESS_BLOCK == 0x40000002);
}

/* Test service flags */
static void test_service_flags(void) {
    assert(SERVICE_NODE_NETWORK == (1 << 0));
    assert(SERVICE_NODE_WITNESS == (1 << 3));
    assert(SERVICE_NODE_NETWORK_LIMITED == (1 << 10));
}

/* Test reject codes */
static void test_reject_codes(void) {
    assert(REJECT_MALFORMED == 0x01);
    assert(REJECT_INVALID == 0x10);
    assert(REJECT_OBSOLETE == 0x11);
    assert(REJECT_DUPLICATE == 0x12);
    assert(REJECT_NONSTANDARD == 0x40);
    assert(REJECT_DUST == 0x41);
    assert(REJECT_INSUFFICIENTFEE == 0x42);
    assert(REJECT_CHECKPOINT == 0x43);
}

/* Test network magic bytes */
static void test_magic_bytes(void) {
    assert(MAGIC_MAINNET == 0xD9B4BEF9);
    assert(MAGIC_TESTNET == 0x0709110B);
    assert(MAGIC_REGTEST == 0xDAB5BFFA);
}

/* Test protocol constants */
static void test_protocol_constants(void) {
    assert(PROTOCOL_VERSION == 70016);
    assert(MAX_MESSAGE_SIZE == (32 * 1024 * 1024));
    assert(MAX_INV_ENTRIES == 50000);
    assert(MAX_HEADERS_COUNT == 2000);
    assert(MAX_ADDR_COUNT == 1000);
}

int main(void) {
    test_suite_begin("Protocol Message Tests");

    test_section("Command Parsing");
    test_case("Parse all valid commands"); test_msg_parse_command_valid(); test_pass();
    test_case("Parse invalid commands"); test_msg_parse_command_invalid(); test_pass();
    test_case("Reject command without null terminator"); test_msg_parse_command_no_null(); test_pass();
    test_case("Parse command with null padding"); test_msg_parse_command_padded(); test_pass();

    test_section("Command String Retrieval");
    test_case("Get all command strings"); test_msg_command_string(); test_pass();
    test_case("Get unknown command string"); test_msg_command_string_unknown(); test_pass();

    test_section("Checksum Computation");
    test_case("Checksum of empty payload"); test_msg_checksum_empty(); test_pass();
    test_case("Checksum of known payload"); test_msg_checksum_known(); test_pass();
    test_case("Checksum of verack message"); test_msg_checksum_verack(); test_pass();

    test_section("Header Validation");
    test_case("Valid mainnet header"); test_msg_header_valid_mainnet(); test_pass();
    test_case("Valid testnet header"); test_msg_header_valid_testnet(); test_pass();
    test_case("Valid regtest header"); test_msg_header_valid_regtest(); test_pass();
    test_case("Reject invalid magic"); test_msg_header_invalid_magic(); test_pass();
    test_case("Reject missing null terminator"); test_msg_header_no_null_terminator(); test_pass();
    test_case("Reject oversized payload"); test_msg_header_oversized_payload(); test_pass();
    test_case("Accept max size payload"); test_msg_header_max_size_payload(); test_pass();
    test_case("Accept zero length payload"); test_msg_header_zero_length(); test_pass();
    test_case("Header size is 24 bytes"); test_msg_header_size(); test_pass();
    test_case("Command null padding"); test_command_padding(); test_pass();

    test_section("Round-trip Conversion");
    test_case("Command string round-trip"); test_command_roundtrip(); test_pass();

    test_section("Protocol Constants");
    test_case("Inventory types"); test_inv_types(); test_pass();
    test_case("Service flags"); test_service_flags(); test_pass();
    test_case("Reject codes"); test_reject_codes(); test_pass();
    test_case("Network magic bytes"); test_magic_bytes(); test_pass();
    test_case("Protocol constants"); test_protocol_constants(); test_pass();

    test_suite_end();
    return test_global_summary();
}
