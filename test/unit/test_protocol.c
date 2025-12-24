/**
 * Unit tests for protocol message structures
 */

#include "protocol.h"
#include "sha256.h"
#include <stdio.h>
#include <string.h>
#include "test_utils.h"


/* Test command parsing */
static void test_msg_parse_command_valid(void) {
    test_case("Parse all valid commands");

    /* Test all valid commands */
    if (msg_parse_command("version") != MSG_VERSION) { test_fail("version"); return; }
    if (msg_parse_command("verack") != MSG_VERACK) { test_fail("verack"); return; }
    if (msg_parse_command("ping") != MSG_PING) { test_fail("ping"); return; }
    if (msg_parse_command("pong") != MSG_PONG) { test_fail("pong"); return; }
    if (msg_parse_command("inv") != MSG_INV) { test_fail("inv"); return; }
    if (msg_parse_command("getdata") != MSG_GETDATA) { test_fail("getdata"); return; }
    if (msg_parse_command("notfound") != MSG_NOTFOUND) { test_fail("notfound"); return; }
    if (msg_parse_command("block") != MSG_BLOCK) { test_fail("block"); return; }
    if (msg_parse_command("tx") != MSG_TX) { test_fail("tx"); return; }
    if (msg_parse_command("addr") != MSG_ADDR) { test_fail("addr"); return; }
    if (msg_parse_command("getaddr") != MSG_GETADDR) { test_fail("getaddr"); return; }
    if (msg_parse_command("getheaders") != MSG_GETHEADERS) { test_fail("getheaders"); return; }
    if (msg_parse_command("getblocks") != MSG_GETBLOCKS) { test_fail("getblocks"); return; }
    if (msg_parse_command("headers") != MSG_HEADERS) { test_fail("headers"); return; }
    if (msg_parse_command("reject") != MSG_REJECT) { test_fail("reject"); return; }
    if (msg_parse_command("sendheaders") != MSG_SENDHEADERS) { test_fail("sendheaders"); return; }
    if (msg_parse_command("feefilter") != MSG_FEEFILTER) { test_fail("feefilter"); return; }
    if (msg_parse_command("sendcmpct") != MSG_SENDCMPCT) { test_fail("sendcmpct"); return; }
    if (msg_parse_command("wtxidrelay") != MSG_WTXIDRELAY) { test_fail("wtxidrelay"); return; }

    test_pass();
}

static void test_msg_parse_command_invalid(void) {
    test_case("Parse invalid commands");

    /* Test unknown commands */
    if (msg_parse_command("unknown") != MSG_UNKNOWN) { test_fail("unknown"); return; }
    if (msg_parse_command("xyz") != MSG_UNKNOWN) { test_fail("xyz"); return; }
    if (msg_parse_command("") != MSG_UNKNOWN) { test_fail("empty string"); return; }

    test_pass();
}

static void test_msg_parse_command_no_null(void) {
    test_case("Reject command without null terminator");

    /* Command without null terminator should be rejected */
    char no_null[COMMAND_LEN];
    memset(no_null, 'x', COMMAND_LEN);
    if (msg_parse_command(no_null) != MSG_UNKNOWN) {
        test_fail("should reject non-null-terminated command");
        return;
    }

    test_pass();
}

static void test_msg_parse_command_padded(void) {
    test_case("Parse command with null padding");

    /* Command with null padding (standard wire format) */
    char padded[COMMAND_LEN];
    memset(padded, 0, COMMAND_LEN);
    strcpy(padded, "version");
    if (msg_parse_command(padded) != MSG_VERSION) {
        test_fail("failed to parse padded command");
        return;
    }

    test_pass();
}

/* Test command string retrieval */
static void test_msg_command_string(void) {
    test_case("Get all command strings");

    if (strcmp(msg_command_string(MSG_VERSION), "version") != 0) { test_fail("version"); return; }
    if (strcmp(msg_command_string(MSG_VERACK), "verack") != 0) { test_fail("verack"); return; }
    if (strcmp(msg_command_string(MSG_PING), "ping") != 0) { test_fail("ping"); return; }
    if (strcmp(msg_command_string(MSG_PONG), "pong") != 0) { test_fail("pong"); return; }
    if (strcmp(msg_command_string(MSG_INV), "inv") != 0) { test_fail("inv"); return; }
    if (strcmp(msg_command_string(MSG_GETDATA), "getdata") != 0) { test_fail("getdata"); return; }
    if (strcmp(msg_command_string(MSG_NOTFOUND), "notfound") != 0) { test_fail("notfound"); return; }
    if (strcmp(msg_command_string(MSG_BLOCK), "block") != 0) { test_fail("block"); return; }
    if (strcmp(msg_command_string(MSG_TX), "tx") != 0) { test_fail("tx"); return; }
    if (strcmp(msg_command_string(MSG_ADDR), "addr") != 0) { test_fail("addr"); return; }
    if (strcmp(msg_command_string(MSG_GETADDR), "getaddr") != 0) { test_fail("getaddr"); return; }
    if (strcmp(msg_command_string(MSG_GETHEADERS), "getheaders") != 0) { test_fail("getheaders"); return; }
    if (strcmp(msg_command_string(MSG_GETBLOCKS), "getblocks") != 0) { test_fail("getblocks"); return; }
    if (strcmp(msg_command_string(MSG_HEADERS), "headers") != 0) { test_fail("headers"); return; }
    if (strcmp(msg_command_string(MSG_REJECT), "reject") != 0) { test_fail("reject"); return; }
    if (strcmp(msg_command_string(MSG_SENDHEADERS), "sendheaders") != 0) { test_fail("sendheaders"); return; }
    if (strcmp(msg_command_string(MSG_FEEFILTER), "feefilter") != 0) { test_fail("feefilter"); return; }
    if (strcmp(msg_command_string(MSG_SENDCMPCT), "sendcmpct") != 0) { test_fail("sendcmpct"); return; }
    if (strcmp(msg_command_string(MSG_WTXIDRELAY), "wtxidrelay") != 0) { test_fail("wtxidrelay"); return; }

    test_pass();
}

static void test_msg_command_string_unknown(void) {
    test_case("Get unknown command string");

    /* Unknown type should return NULL */
    if (msg_command_string(MSG_UNKNOWN) != NULL) {
        test_fail("MSG_UNKNOWN should return NULL");
        return;
    }
    if (msg_command_string((msg_type_t)999) != NULL) {
        test_fail("Unknown type 999 should return NULL");
        return;
    }

    test_pass();
}

/* Test checksum computation */
static void test_msg_checksum_empty(void) {
    test_case("Checksum of empty payload");

    /* Empty payload */
    uint32_t checksum = msg_checksum(NULL, 0);

    /* SHA256d of empty string:
     * SHA256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
     * SHA256(e3b0c442...) = 5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456
     * First 4 bytes (LE): 0xe2e0f65d
     */
    if (checksum != 0xe2e0f65d) {
        test_fail_uint("checksum mismatch", 0xe2e0f65d, checksum);
        return;
    }

    test_pass();
}

static void test_msg_checksum_known(void) {
    test_case("Checksum of known payload");

    /* Known payload: "hello" */
    uint8_t payload[] = "hello";
    uint32_t checksum = msg_checksum(payload, 5);

    /* SHA256d("hello"):
     * SHA256("hello") = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
     * SHA256(2cf24dba...) = 9595c9df90075148eb06860365df33584b75bff782a510c6cd4883a419833d50
     * First 4 bytes (LE): 0xdfc99595
     */
    if (checksum != 0xdfc99595) {
        test_fail_uint("checksum mismatch", 0xdfc99595, checksum);
        return;
    }

    test_pass();
}

static void test_msg_checksum_verack(void) {
    test_case("Checksum of verack message");

    /* Verack message has empty payload, same as empty test */
    uint32_t checksum = msg_checksum(NULL, 0);
    if (checksum != 0xe2e0f65d) {
        test_fail_uint("checksum mismatch", 0xe2e0f65d, checksum);
        return;
    }

    test_pass();
}

/* Test header validation */
static void test_msg_header_valid_mainnet(void) {
    test_case("Valid mainnet header");

    msg_header_t header;
    header.magic = MAGIC_MAINNET;
    strcpy(header.command, "version");
    header.length = 100;
    header.checksum = 0x12345678;

    if (msg_header_valid(&header, MAGIC_MAINNET) != ECHO_TRUE) {
        test_fail("mainnet header validation failed");
        return;
    }

    test_pass();
}

static void test_msg_header_valid_testnet(void) {
    test_case("Valid testnet header");

    msg_header_t header;
    header.magic = MAGIC_TESTNET;
    strcpy(header.command, "ping");
    header.length = 8;
    header.checksum = 0x12345678;

    if (msg_header_valid(&header, MAGIC_TESTNET) != ECHO_TRUE) {
        test_fail("testnet header validation failed");
        return;
    }

    test_pass();
}

static void test_msg_header_valid_regtest(void) {
    test_case("Valid regtest header");

    msg_header_t header;
    header.magic = MAGIC_REGTEST;
    strcpy(header.command, "pong");
    header.length = 8;
    header.checksum = 0x12345678;

    if (msg_header_valid(&header, MAGIC_REGTEST) != ECHO_TRUE) {
        test_fail("regtest header validation failed");
        return;
    }

    test_pass();
}

static void test_msg_header_invalid_magic(void) {
    test_case("Reject invalid magic");

    msg_header_t header;
    header.magic = 0xDEADBEEF;  /* Wrong magic */
    strcpy(header.command, "version");
    header.length = 100;
    header.checksum = 0x12345678;

    if (msg_header_valid(&header, MAGIC_MAINNET) != ECHO_FALSE) {
        test_fail("should reject invalid magic");
        return;
    }

    test_pass();
}

static void test_msg_header_no_null_terminator(void) {
    test_case("Reject missing null terminator");

    msg_header_t header;
    header.magic = MAGIC_MAINNET;
    memset(header.command, 'x', COMMAND_LEN);  /* No null terminator */
    header.length = 100;
    header.checksum = 0x12345678;

    if (msg_header_valid(&header, MAGIC_MAINNET) != ECHO_FALSE) {
        test_fail("should reject command without null terminator");
        return;
    }

    test_pass();
}

static void test_msg_header_oversized_payload(void) {
    test_case("Reject oversized payload");

    msg_header_t header;
    header.magic = MAGIC_MAINNET;
    strcpy(header.command, "block");
    header.length = MAX_MESSAGE_SIZE + 1;  /* Too large */
    header.checksum = 0x12345678;

    if (msg_header_valid(&header, MAGIC_MAINNET) != ECHO_FALSE) {
        test_fail("should reject oversized payload");
        return;
    }

    test_pass();
}

static void test_msg_header_max_size_payload(void) {
    test_case("Accept max size payload");

    msg_header_t header;
    header.magic = MAGIC_MAINNET;
    strcpy(header.command, "block");
    header.length = MAX_MESSAGE_SIZE;  /* Exactly at limit */
    header.checksum = 0x12345678;

    if (msg_header_valid(&header, MAGIC_MAINNET) != ECHO_TRUE) {
        test_fail("should accept max size payload");
        return;
    }

    test_pass();
}

static void test_msg_header_zero_length(void) {
    test_case("Accept zero length payload");

    msg_header_t header;
    header.magic = MAGIC_MAINNET;
    strcpy(header.command, "verack");
    header.length = 0;  /* Empty payload */
    header.checksum = 0xe2e0f65d;

    if (msg_header_valid(&header, MAGIC_MAINNET) != ECHO_TRUE) {
        test_fail("should accept zero length payload");
        return;
    }

    test_pass();
}

/* Test message header size */
static void test_msg_header_size(void) {
    test_case("Header size is 24 bytes");

    /* Header should be exactly 24 bytes */
    if (sizeof(msg_header_t) != 24) {
        test_fail_uint("header size mismatch", 24, sizeof(msg_header_t));
        return;
    }

    test_pass();
}

/* Test command padding */
static void test_command_padding(void) {
    test_case("Command null padding");

    msg_header_t header;
    header.magic = MAGIC_MAINNET;

    /* Clear command buffer */
    memset(header.command, 0, COMMAND_LEN);

    /* Set short command */
    strcpy(header.command, "ping");

    /* Remaining bytes should be zero (null padding) */
    for (size_t i = 5; i < COMMAND_LEN; i++) {
        if (header.command[i] != 0) {
            test_fail("command padding not null");
            return;
        }
    }

    header.length = 8;
    header.checksum = 0x12345678;

    if (msg_header_valid(&header, MAGIC_MAINNET) != ECHO_TRUE) {
        test_fail("header validation failed");
        return;
    }

    test_pass();
}

/* Test round-trip command conversion */
static void test_command_roundtrip(void) {
    test_case("Command string round-trip");

    for (msg_type_t type = MSG_VERSION; type < MSG_UNKNOWN; type++) {
        const char *cmd = msg_command_string(type);
        if (cmd == NULL) {
            test_fail("msg_command_string returned NULL");
            return;
        }

        msg_type_t parsed = msg_parse_command(cmd);
        if (parsed != type) {
            test_fail("round-trip conversion failed");
            return;
        }
    }

    test_pass();
}

/* Test inventory type constants */
static void test_inv_types(void) {
    test_case("Inventory types");

    /* Verify standard inventory types */
    if (INV_ERROR != 0) { test_fail("INV_ERROR"); return; }
    if (INV_TX != 1) { test_fail("INV_TX"); return; }
    if (INV_BLOCK != 2) { test_fail("INV_BLOCK"); return; }
    if (INV_FILTERED_BLOCK != 3) { test_fail("INV_FILTERED_BLOCK"); return; }

    /* Witness types have MSG_WITNESS_FLAG set */
    if (INV_WITNESS_TX != 0x40000001) { test_fail("INV_WITNESS_TX"); return; }
    if (INV_WITNESS_BLOCK != 0x40000002) { test_fail("INV_WITNESS_BLOCK"); return; }

    test_pass();
}

/* Test service flags */
static void test_service_flags(void) {
    test_case("Service flags");

    if (SERVICE_NODE_NETWORK != (1 << 0)) { test_fail("SERVICE_NODE_NETWORK"); return; }
    if (SERVICE_NODE_WITNESS != (1 << 3)) { test_fail("SERVICE_NODE_WITNESS"); return; }
    if (SERVICE_NODE_NETWORK_LIMITED != (1 << 10)) { test_fail("SERVICE_NODE_NETWORK_LIMITED"); return; }

    test_pass();
}

/* Test reject codes */
static void test_reject_codes(void) {
    test_case("Reject codes");

    if (REJECT_MALFORMED != 0x01) { test_fail("REJECT_MALFORMED"); return; }
    if (REJECT_INVALID != 0x10) { test_fail("REJECT_INVALID"); return; }
    if (REJECT_OBSOLETE != 0x11) { test_fail("REJECT_OBSOLETE"); return; }
    if (REJECT_DUPLICATE != 0x12) { test_fail("REJECT_DUPLICATE"); return; }
    if (REJECT_NONSTANDARD != 0x40) { test_fail("REJECT_NONSTANDARD"); return; }
    if (REJECT_DUST != 0x41) { test_fail("REJECT_DUST"); return; }
    if (REJECT_INSUFFICIENTFEE != 0x42) { test_fail("REJECT_INSUFFICIENTFEE"); return; }
    if (REJECT_CHECKPOINT != 0x43) { test_fail("REJECT_CHECKPOINT"); return; }

    test_pass();
}

/* Test network magic bytes */
static void test_magic_bytes(void) {
    test_case("Network magic bytes");

    if (MAGIC_MAINNET != 0xD9B4BEF9) { test_fail("MAGIC_MAINNET"); return; }
    if (MAGIC_TESTNET != 0x0709110B) { test_fail("MAGIC_TESTNET"); return; }
    if (MAGIC_REGTEST != 0xDAB5BFFA) { test_fail("MAGIC_REGTEST"); return; }

    test_pass();
}

/* Test protocol constants */
static void test_protocol_constants(void) {
    test_case("Protocol constants");

    if (PROTOCOL_VERSION != 70016) { test_fail("PROTOCOL_VERSION"); return; }
    if (MAX_MESSAGE_SIZE != (32 * 1024 * 1024)) { test_fail("MAX_MESSAGE_SIZE"); return; }
    if (MAX_INV_ENTRIES != 50000) { test_fail("MAX_INV_ENTRIES"); return; }
    if (MAX_HEADERS_COUNT != 2000) { test_fail("MAX_HEADERS_COUNT"); return; }
    if (MAX_ADDR_COUNT != 1000) { test_fail("MAX_ADDR_COUNT"); return; }

    test_pass();
}

int main(void) {
    test_suite_begin("Protocol Message Tests");

    test_section("Command Parsing");
    test_msg_parse_command_valid();
    test_msg_parse_command_invalid();
    test_msg_parse_command_no_null();
    test_msg_parse_command_padded();

    test_section("Command String Retrieval");
    test_msg_command_string();
    test_msg_command_string_unknown();

    test_section("Checksum Computation");
    test_msg_checksum_empty();
    test_msg_checksum_known();
    test_msg_checksum_verack();

    test_section("Header Validation");
    test_msg_header_valid_mainnet();
    test_msg_header_valid_testnet();
    test_msg_header_valid_regtest();
    test_msg_header_invalid_magic();
    test_msg_header_no_null_terminator();
    test_msg_header_oversized_payload();
    test_msg_header_max_size_payload();
    test_msg_header_zero_length();
    test_msg_header_size();
    test_command_padding();

    test_section("Round-trip Conversion");
    test_command_roundtrip();

    test_section("Protocol Constants");
    test_inv_types();
    test_service_flags();
    test_reject_codes();
    test_magic_bytes();
    test_protocol_constants();

    test_suite_end();
    return test_global_summary();
}
