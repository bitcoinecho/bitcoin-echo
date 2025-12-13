/**
 * Bitcoin Echo — Protocol Message Serialization Tests
 *
 * Unit tests for P2P protocol message serialization and deserialization.
 */

#include "protocol.h"
#include "protocol_serialize.h"
#include "sha256.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "test_utils.h"

#define PASS() \
    do { \
        printf("  PASS\n"); \
        pass_count++; \
    } while(0)

#define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return; \
        } \
    } while(0)

/* ========================================================================
 * Helper Primitives Tests
 * ======================================================================== */

static void test_read_write_primitives(void) {
    uint8_t buf[128];
    uint8_t *ptr = buf;
    const uint8_t *end = buf + sizeof(buf);

    /* Test u8 */
    ASSERT(write_u8(&ptr, end, 0x42) == ECHO_OK);
    ASSERT(ptr - buf == 1);

    /* Test u16_le */
    ASSERT(write_u16_le(&ptr, end, 0x1234) == ECHO_OK);
    ASSERT(ptr - buf == 3);
    ASSERT(buf[1] == 0x34);
    ASSERT(buf[2] == 0x12);

    /* Test u16_be (for ports) */
    ASSERT(write_u16_be(&ptr, end, 0x5678) == ECHO_OK);
    ASSERT(ptr - buf == 5);
    ASSERT(buf[3] == 0x56);
    ASSERT(buf[4] == 0x78);

    /* Test u32_le */
    ASSERT(write_u32_le(&ptr, end, 0x12345678) == ECHO_OK);
    ASSERT(ptr - buf == 9);
    ASSERT(buf[5] == 0x78);
    ASSERT(buf[8] == 0x12);

    /* Test u64_le */
    ASSERT(write_u64_le(&ptr, end, 0x123456789ABCDEF0ULL) == ECHO_OK);
    ASSERT(ptr - buf == 17);
    ASSERT(buf[9] == 0xF0);
    ASSERT(buf[16] == 0x12);

    /* Test reading back */
    const uint8_t *rptr = buf;
    uint8_t v8;
    uint16_t v16;
    uint32_t v32;
    uint64_t v64;

    ASSERT(read_u8(&rptr, end, &v8) == ECHO_OK);
    ASSERT(v8 == 0x42);

    ASSERT(read_u16_le(&rptr, end, &v16) == ECHO_OK);
    ASSERT(v16 == 0x1234);

    ASSERT(read_u16_be(&rptr, end, &v16) == ECHO_OK);
    ASSERT(v16 == 0x5678);

    ASSERT(read_u32_le(&rptr, end, &v32) == ECHO_OK);
    ASSERT(v32 == 0x12345678);

    ASSERT(read_u64_le(&rptr, end, &v64) == ECHO_OK);
    ASSERT(v64 == 0x123456789ABCDEF0ULL);

    test_pass();
}

static void test_buffer_overflow(void) {
    uint8_t buf[4];
    uint8_t *ptr = buf;
    const uint8_t *end = buf + sizeof(buf);

    /* Should succeed */
    ASSERT(write_u32_le(&ptr, end, 0x12345678) == ECHO_OK);

    /* Should fail - buffer full */
    ASSERT(write_u8(&ptr, end, 0x42) == ECHO_ERR_BUFFER_TOO_SMALL);

    /* Reading past end should fail */
    const uint8_t *rptr = buf;
    uint32_t v32;
    uint8_t v8;
    ASSERT(read_u32_le(&rptr, end, &v32) == ECHO_OK);
    ASSERT(read_u8(&rptr, end, &v8) == ECHO_ERR_TRUNCATED);

    test_pass();
}

/* ========================================================================
 * Message Header Tests
 * ======================================================================== */

static void test_msg_header_serialize(void) {
    msg_header_t header;
    header.magic = MAGIC_MAINNET;
    memset(header.command, 0, COMMAND_LEN);
    strcpy(header.command, "version");
    header.length = 102;
    header.checksum = 0x12345678;

    uint8_t buf[24];
    ASSERT(msg_header_serialize(&header, buf, sizeof(buf)) == ECHO_OK);

    /* Verify magic */
    ASSERT(buf[0] == 0xF9);
    ASSERT(buf[1] == 0xBE);
    ASSERT(buf[2] == 0xB4);
    ASSERT(buf[3] == 0xD9);

    /* Verify command */
    ASSERT(memcmp(buf + 4, "version", 7) == 0);

    /* Verify length */
    uint32_t len = buf[16] | (buf[17] << 8) | (buf[18] << 16) | (buf[19] << 24);
    ASSERT(len == 102);

    /* Verify checksum */
    uint32_t cksum = buf[20] | (buf[21] << 8) | (buf[22] << 16) | (buf[23] << 24);
    ASSERT(cksum == 0x12345678);

    test_pass();
}

static void test_msg_header_deserialize(void) {
    uint8_t buf[24] = {
        0xF9, 0xBE, 0xB4, 0xD9,  /* magic */
        'p', 'i', 'n', 'g', 0, 0, 0, 0, 0, 0, 0, 0,  /* command */
        0x08, 0x00, 0x00, 0x00,  /* length = 8 */
        0xAB, 0xCD, 0xEF, 0x12   /* checksum */
    };

    msg_header_t header;
    ASSERT(msg_header_deserialize(buf, sizeof(buf), &header) == ECHO_OK);

    ASSERT(header.magic == MAGIC_MAINNET);
    ASSERT(strcmp(header.command, "ping") == 0);
    ASSERT(header.length == 8);
    ASSERT(header.checksum == 0x12EFCDAB);

    test_pass();
}

static void test_msg_header_roundtrip(void) {
    msg_header_t original;
    original.magic = MAGIC_TESTNET;
    memset(original.command, 0, COMMAND_LEN);
    strcpy(original.command, "getaddr");
    original.length = 0;
    original.checksum = 0x5DF6E0E2;

    uint8_t buf[24];
    ASSERT(msg_header_serialize(&original, buf, sizeof(buf)) == ECHO_OK);

    msg_header_t parsed;
    ASSERT(msg_header_deserialize(buf, sizeof(buf), &parsed) == ECHO_OK);

    ASSERT(parsed.magic == original.magic);
    ASSERT(strcmp(parsed.command, original.command) == 0);
    ASSERT(parsed.length == original.length);
    ASSERT(parsed.checksum == original.checksum);

    test_pass();
}

/* ========================================================================
 * ping/pong Message Tests
 * ======================================================================== */

static void test_msg_ping(void) {
    msg_ping_t ping;
    ping.nonce = 0x0123456789ABCDEFULL;

    uint8_t buf[8];
    size_t written;
    ASSERT(msg_ping_serialize(&ping, buf, sizeof(buf), &written) == ECHO_OK);
    ASSERT(written == 8);

    /* Verify little-endian encoding */
    ASSERT(buf[0] == 0xEF);
    ASSERT(buf[7] == 0x01);

    /* Deserialize */
    msg_ping_t parsed;
    size_t consumed;
    ASSERT(msg_ping_deserialize(buf, sizeof(buf), &parsed, &consumed) == ECHO_OK);
    ASSERT(consumed == 8);
    ASSERT(parsed.nonce == 0x0123456789ABCDEFULL);

    test_pass();
}

static void test_msg_pong(void) {
    msg_pong_t pong;
    pong.nonce = 0xFEDCBA9876543210ULL;

    uint8_t buf[8];
    size_t written;
    ASSERT(msg_pong_serialize(&pong, buf, sizeof(buf), &written) == ECHO_OK);
    ASSERT(written == 8);

    msg_pong_t parsed;
    size_t consumed;
    ASSERT(msg_pong_deserialize(buf, sizeof(buf), &parsed, &consumed) == ECHO_OK);
    ASSERT(consumed == 8);
    ASSERT(parsed.nonce == 0xFEDCBA9876543210ULL);

    test_pass();
}

/* ========================================================================
 * version Message Tests
 * ======================================================================== */

static void test_msg_version(void) {
    msg_version_t version;
    version.version = 70016;
    version.services = SERVICE_NODE_NETWORK | SERVICE_NODE_WITNESS;
    version.timestamp = 1234567890;

    /* Receiver address */
    version.addr_recv.services = SERVICE_NODE_NETWORK;
    memset(version.addr_recv.ip, 0, 16);
    version.addr_recv.ip[10] = 0xFF;
    version.addr_recv.ip[11] = 0xFF;
    version.addr_recv.ip[12] = 192;
    version.addr_recv.ip[13] = 168;
    version.addr_recv.ip[14] = 1;
    version.addr_recv.ip[15] = 1;
    version.addr_recv.port = 8333;

    /* Sender address */
    version.addr_from.services = SERVICE_NODE_NETWORK;
    memset(version.addr_from.ip, 0, 16);
    version.addr_from.port = 0;

    version.nonce = 0x123456789ABCDEF0ULL;
    strcpy(version.user_agent, "/BitcoinEcho:0.1.0/");
    version.user_agent_len = strlen(version.user_agent);
    version.start_height = 700000;
    version.relay = ECHO_TRUE;

    uint8_t buf[256];
    size_t written;
    ASSERT(msg_version_serialize(&version, buf, sizeof(buf), &written) == ECHO_OK);
    ASSERT(written > 85); /* Minimum version message size */

    /* Deserialize */
    msg_version_t parsed;
    size_t consumed;
    ASSERT(msg_version_deserialize(buf, written, &parsed, &consumed) == ECHO_OK);
    ASSERT(consumed == written);
    ASSERT(parsed.version == 70016);
    ASSERT(parsed.services == (SERVICE_NODE_NETWORK | SERVICE_NODE_WITNESS));
    ASSERT(parsed.timestamp == 1234567890);
    ASSERT(parsed.nonce == 0x123456789ABCDEF0ULL);
    ASSERT(strcmp(parsed.user_agent, "/BitcoinEcho:0.1.0/") == 0);
    ASSERT(parsed.start_height == 700000);
    ASSERT(parsed.relay == ECHO_TRUE);

    test_pass();
}

static void test_msg_version_old_protocol(void) {

    msg_version_t version;
    version.version = 60000; /* Old version < 70001 */
    version.services = SERVICE_NODE_NETWORK;
    version.timestamp = 1234567890;

    memset(&version.addr_recv, 0, sizeof(version.addr_recv));
    memset(&version.addr_from, 0, sizeof(version.addr_from));

    version.nonce = 0xABCDEF;
    strcpy(version.user_agent, "/OldClient:0.1/");
    version.user_agent_len = strlen(version.user_agent);
    version.start_height = 100000;
    version.relay = ECHO_FALSE; /* Should not be serialized */

    uint8_t buf[256];
    size_t written;
    ASSERT(msg_version_serialize(&version, buf, sizeof(buf), &written) == ECHO_OK);

    /* Deserialize - should default relay to TRUE */
    msg_version_t parsed;
    size_t consumed;
    ASSERT(msg_version_deserialize(buf, written, &parsed, &consumed) == ECHO_OK);
    ASSERT(parsed.relay == ECHO_TRUE); /* Default for old versions */

    test_pass();
}

/* ========================================================================
 * inv Message Tests
 * ======================================================================== */

static void test_msg_inv(void) {
    inv_vector_t inventory[3];

    /* TX inventory */
    inventory[0].type = INV_TX;
    memset(inventory[0].hash.bytes, 0xAA, 32);

    /* Block inventory */
    inventory[1].type = INV_BLOCK;
    memset(inventory[1].hash.bytes, 0xBB, 32);

    /* Witness TX */
    inventory[2].type = INV_WITNESS_TX;
    memset(inventory[2].hash.bytes, 0xCC, 32);

    msg_inv_t inv;
    inv.count = 3;
    inv.inventory = inventory;

    uint8_t buf[512];
    size_t written;
    ASSERT(msg_inv_serialize(&inv, buf, sizeof(buf), &written) == ECHO_OK);

    /* Should be: 1 byte varint (count=3) + 3 * 36 bytes (type + hash) */
    ASSERT(written == 1 + 3 * 36);

    /* Deserialize count only (caller allocates inventory) */
    msg_inv_t parsed;
    size_t consumed;
    ASSERT(msg_inv_deserialize(buf, written, &parsed, &consumed) == ECHO_OK);
    ASSERT(parsed.count == 3);
    ASSERT(consumed == written);

    test_pass();
}

static void test_msg_inv_empty(void) {
    msg_inv_t inv;
    inv.count = 0;
    inv.inventory = NULL;

    uint8_t buf[16];
    size_t written;
    ASSERT(msg_inv_serialize(&inv, buf, sizeof(buf), &written) == ECHO_OK);
    ASSERT(written == 1); /* Just the varint count */

    msg_inv_t parsed;
    size_t consumed;
    ASSERT(msg_inv_deserialize(buf, written, &parsed, &consumed) == ECHO_OK);
    ASSERT(parsed.count == 0);

    test_pass();
}

/* ========================================================================
 * addr Message Tests
 * ======================================================================== */

static void test_msg_addr(void) {
    net_addr_t addresses[2];

    addresses[0].timestamp = 1609459200; /* 2021-01-01 */
    addresses[0].services = SERVICE_NODE_NETWORK | SERVICE_NODE_WITNESS;
    memset(addresses[0].ip, 0, 16);
    addresses[0].ip[10] = 0xFF;
    addresses[0].ip[11] = 0xFF;
    addresses[0].ip[12] = 192;
    addresses[0].ip[13] = 168;
    addresses[0].ip[14] = 1;
    addresses[0].ip[15] = 100;
    addresses[0].port = 8333;

    addresses[1].timestamp = 1609459300;
    addresses[1].services = SERVICE_NODE_NETWORK;
    memset(addresses[1].ip, 0, 16);
    addresses[1].ip[10] = 0xFF;
    addresses[1].ip[11] = 0xFF;
    addresses[1].ip[12] = 10;
    addresses[1].ip[13] = 0;
    addresses[1].ip[14] = 0;
    addresses[1].ip[15] = 1;
    addresses[1].port = 18333;

    msg_addr_t addr;
    addr.count = 2;
    addr.addresses = addresses;

    uint8_t buf[256];
    size_t written;
    ASSERT(msg_addr_serialize(&addr, buf, sizeof(buf), &written) == ECHO_OK);

    /* 1 byte varint + 2 * 30 bytes per address */
    ASSERT(written == 1 + 2 * 30);

    msg_addr_t parsed;
    size_t consumed;
    ASSERT(msg_addr_deserialize(buf, written, &parsed, &consumed) == ECHO_OK);
    ASSERT(parsed.count == 2);
    ASSERT(consumed == written);

    test_pass();
}

/* ========================================================================
 * getheaders Message Tests
 * ======================================================================== */

static void test_msg_getheaders(void) {
    hash256_t locator[3];
    memset(locator[0].bytes, 0x11, 32);
    memset(locator[1].bytes, 0x22, 32);
    memset(locator[2].bytes, 0x33, 32);

    hash256_t hash_stop;
    memset(hash_stop.bytes, 0, 32); /* Request as many as possible */

    msg_getheaders_t getheaders;
    getheaders.version = PROTOCOL_VERSION;
    getheaders.hash_count = 3;
    getheaders.block_locator = locator;
    getheaders.hash_stop = hash_stop;

    uint8_t buf[512];
    size_t written;
    ASSERT(msg_getheaders_serialize(&getheaders, buf, sizeof(buf), &written) == ECHO_OK);

    /* 4 bytes version + 1 byte varint + 3*32 bytes hashes + 32 bytes stop */
    ASSERT(written == 4 + 1 + 96 + 32);

    msg_getheaders_t parsed;
    size_t consumed;
    ASSERT(msg_getheaders_deserialize(buf, written, &parsed, &consumed) == ECHO_OK);
    ASSERT(parsed.version == PROTOCOL_VERSION);
    ASSERT(parsed.hash_count == 3);
    ASSERT(consumed == written);

    test_pass();
}

/* ========================================================================
 * feefilter Message Tests
 * ======================================================================== */

static void test_msg_feefilter(void) {
    msg_feefilter_t feefilter;
    feefilter.feerate = 1000; /* 1000 satoshis per 1000 bytes */

    uint8_t buf[8];
    size_t written;
    ASSERT(msg_feefilter_serialize(&feefilter, buf, sizeof(buf), &written) == ECHO_OK);
    ASSERT(written == 8);

    msg_feefilter_t parsed;
    size_t consumed;
    ASSERT(msg_feefilter_deserialize(buf, sizeof(buf), &parsed, &consumed) == ECHO_OK);
    ASSERT(consumed == 8);
    ASSERT(parsed.feerate == 1000);

    test_pass();
}

/* ========================================================================
 * sendcmpct Message Tests
 * ======================================================================== */

static void test_msg_sendcmpct(void) {
    msg_sendcmpct_t sendcmpct;
    sendcmpct.announce = ECHO_TRUE;
    sendcmpct.version = 2;

    uint8_t buf[9];
    size_t written;
    ASSERT(msg_sendcmpct_serialize(&sendcmpct, buf, sizeof(buf), &written) == ECHO_OK);
    ASSERT(written == 9);

    ASSERT(buf[0] == 1); /* announce = true */

    msg_sendcmpct_t parsed;
    size_t consumed;
    ASSERT(msg_sendcmpct_deserialize(buf, sizeof(buf), &parsed, &consumed) == ECHO_OK);
    ASSERT(consumed == 9);
    ASSERT(parsed.announce == ECHO_TRUE);
    ASSERT(parsed.version == 2);

    test_pass();
}

/* ========================================================================
 * reject Message Tests
 * ======================================================================== */

static void test_msg_reject(void) {
    msg_reject_t reject;
    strcpy(reject.message, "tx");
    reject.ccode = REJECT_INVALID;
    strcpy(reject.reason, "bad-txns-inputs-missingorspent");
    reject.reason_len = strlen(reject.reason);
    memset(reject.data.bytes, 0xAB, 32);
    reject.has_data = ECHO_TRUE;

    uint8_t buf[256];
    size_t written;
    ASSERT(msg_reject_serialize(&reject, buf, sizeof(buf), &written) == ECHO_OK);

    msg_reject_t parsed;
    size_t consumed;
    ASSERT(msg_reject_deserialize(buf, written, &parsed, &consumed) == ECHO_OK);
    ASSERT(consumed == written);
    ASSERT(strcmp(parsed.message, "tx") == 0);
    ASSERT(parsed.ccode == REJECT_INVALID);
    ASSERT(strcmp(parsed.reason, "bad-txns-inputs-missingorspent") == 0);
    ASSERT(parsed.has_data == ECHO_TRUE);
    ASSERT(memcmp(parsed.data.bytes, reject.data.bytes, 32) == 0);

    test_pass();
}

static void test_msg_reject_no_data(void) {
    msg_reject_t reject;
    strcpy(reject.message, "block");
    reject.ccode = REJECT_DUPLICATE;
    strcpy(reject.reason, "duplicate");
    reject.reason_len = strlen(reject.reason);
    reject.has_data = ECHO_FALSE;

    uint8_t buf[256];
    size_t written;
    ASSERT(msg_reject_serialize(&reject, buf, sizeof(buf), &written) == ECHO_OK);

    msg_reject_t parsed;
    size_t consumed;
    ASSERT(msg_reject_deserialize(buf, written, &parsed, &consumed) == ECHO_OK);
    ASSERT(parsed.has_data == ECHO_FALSE);

    test_pass();
}

/* ========================================================================
 * Main Test Runner
 * ======================================================================== */

int main(void) {
    test_suite_begin("Protocol Serialization Tests");

    test_case("Read write primitives"); test_read_write_primitives(); test_pass();
    test_case("Buffer overflow"); test_buffer_overflow(); test_pass();
    test_case("Msg header serialize"); test_msg_header_serialize(); test_pass();
    test_case("Msg header deserialize"); test_msg_header_deserialize(); test_pass();
    test_case("Msg header roundtrip"); test_msg_header_roundtrip(); test_pass();
    test_case("Msg ping"); test_msg_ping(); test_pass();
    test_case("Msg pong"); test_msg_pong(); test_pass();
    test_case("Msg version"); test_msg_version(); test_pass();
    test_case("Msg version old protocol"); test_msg_version_old_protocol(); test_pass();
    test_case("Msg inv"); test_msg_inv(); test_pass();
    test_case("Msg inv empty"); test_msg_inv_empty(); test_pass();
    test_case("Msg addr"); test_msg_addr(); test_pass();
    test_case("Msg getheaders"); test_msg_getheaders(); test_pass();
    test_case("Msg feefilter"); test_msg_feefilter(); test_pass();
    test_case("Msg sendcmpct"); test_msg_sendcmpct(); test_pass();
    test_case("Msg reject"); test_msg_reject(); test_pass();
    test_case("Msg reject no data"); test_msg_reject_no_data(); test_pass();

    test_suite_end();
    return test_global_summary();
}
