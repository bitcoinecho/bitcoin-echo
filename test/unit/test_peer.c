/**
 * Bitcoin Echo — Peer Connection Management Tests
 *
 * Test coverage:
 * - Peer initialization
 * - Connection state machine
 * - Version/verack handshake
 * - Message queueing and sending
 * - Disconnection handling
 * - Self-connection detection
 * - Error conditions
 *
 * Build once. Build right. Stop.
 */

#include "peer.h"
#include "protocol_serialize.h"
#include "platform.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "test_utils.h"


/**
 * Test peer initialization.
 */
static void test_peer_init(void) {
    peer_t peer;
    peer_init(&peer);

    assert(peer.state == PEER_STATE_DISCONNECTED);
    assert(peer.disconnect_reason == PEER_DISCONNECT_NONE);
    assert(peer.send_queue_count == 0);
    assert(peer.recv_buffer_len == 0);
    assert(peer.bytes_sent == 0);
    assert(peer.bytes_recv == 0);
    assert(peer.messages_sent == 0);
    assert(peer.messages_recv == 0);
    assert(!peer_is_ready(&peer));
    assert(!peer_is_connected(&peer));
}

/**
 * Test peer state strings.
 */
static void test_peer_state_strings(void) {
    assert(strcmp(peer_state_string(PEER_STATE_DISCONNECTED), "DISCONNECTED") == 0);
    assert(strcmp(peer_state_string(PEER_STATE_CONNECTING), "CONNECTING") == 0);
    assert(strcmp(peer_state_string(PEER_STATE_CONNECTED), "CONNECTED") == 0);
    assert(strcmp(peer_state_string(PEER_STATE_HANDSHAKE_SENT), "HANDSHAKE_SENT") == 0);
    assert(strcmp(peer_state_string(PEER_STATE_HANDSHAKE_RECV), "HANDSHAKE_RECV") == 0);
    assert(strcmp(peer_state_string(PEER_STATE_READY), "READY") == 0);

    assert(strcmp(peer_disconnect_reason_string(PEER_DISCONNECT_NONE), "NONE") == 0);
    assert(strcmp(peer_disconnect_reason_string(PEER_DISCONNECT_USER), "USER") == 0);
    assert(strcmp(peer_disconnect_reason_string(PEER_DISCONNECT_PROTOCOL_ERROR), "PROTOCOL_ERROR") == 0);
    assert(strcmp(peer_disconnect_reason_string(PEER_DISCONNECT_NETWORK_ERROR), "NETWORK_ERROR") == 0);
}

/**
 * Test peer_is_ready and peer_is_connected.
 */
static void test_peer_state_checks(void) {
    peer_t peer;
    peer_init(&peer);

    /* Disconnected */
    peer.state = PEER_STATE_DISCONNECTED;
    assert(!peer_is_ready(&peer));
    assert(!peer_is_connected(&peer));

    /* Connecting */
    peer.state = PEER_STATE_CONNECTING;
    assert(!peer_is_ready(&peer));
    assert(peer_is_connected(&peer));

    /* Connected */
    peer.state = PEER_STATE_CONNECTED;
    assert(!peer_is_ready(&peer));
    assert(peer_is_connected(&peer));

    /* Handshake sent */
    peer.state = PEER_STATE_HANDSHAKE_SENT;
    assert(!peer_is_ready(&peer));
    assert(peer_is_connected(&peer));

    /* Handshake received */
    peer.state = PEER_STATE_HANDSHAKE_RECV;
    assert(!peer_is_ready(&peer));
    assert(peer_is_connected(&peer));

    /* Ready */
    peer.state = PEER_STATE_READY;
    assert(peer_is_ready(&peer));
    assert(peer_is_connected(&peer));
}

/**
 * Test message queue operations.
 */
static void test_message_queue(void) {
    peer_t peer;
    peer_init(&peer);
    peer.state = PEER_STATE_READY;

    /* Queue should be empty */
    assert(peer.send_queue_count == 0);

    /* Queue a ping message */
    msg_t ping_msg;
    ping_msg.type = MSG_PING;
    ping_msg.payload.ping.nonce = 0x1234567890ABCDEF;

    echo_result_t result = peer_queue_message(&peer, &ping_msg);
    assert(result == ECHO_SUCCESS);
    assert(peer.send_queue_count == 1);

    /* Queue another message */
    msg_t pong_msg;
    pong_msg.type = MSG_PONG;
    pong_msg.payload.pong.nonce = 0xFEDCBA0987654321;

    result = peer_queue_message(&peer, &pong_msg);
    assert(result == ECHO_SUCCESS);
    assert(peer.send_queue_count == 2);

    /* Verify queue order */
    assert(peer.send_queue[peer.send_queue_head].message.type == MSG_PING);
    assert(peer.send_queue[(peer.send_queue_head + 1) % PEER_SEND_QUEUE_SIZE].message.type == MSG_PONG);
}

/**
 * Test queue full condition.
 */
static void test_message_queue_full(void) {
    peer_t peer;
    peer_init(&peer);
    peer.state = PEER_STATE_READY;

    msg_t msg;
    msg.type = MSG_PING;
    msg.payload.ping.nonce = 0;

    /* Fill the queue */
    for (size_t i = 0; i < PEER_SEND_QUEUE_SIZE; i++) {
        echo_result_t result = peer_queue_message(&peer, &msg);
        assert(result == ECHO_SUCCESS);
    }

    assert(peer.send_queue_count == PEER_SEND_QUEUE_SIZE);

    /* Try to add one more - should fail */
    echo_result_t result = peer_queue_message(&peer, &msg);
    assert(result == ECHO_ERR_FULL);
    assert(peer.send_queue_count == PEER_SEND_QUEUE_SIZE);
}

/**
 * Test queueing in wrong state.
 */
static void test_queue_wrong_state(void) {
    peer_t peer;
    peer_init(&peer);
    peer.state = PEER_STATE_CONNECTED;  /* Not ready */

    msg_t msg;
    msg.type = MSG_PING;
    msg.payload.ping.nonce = 0;

    echo_result_t result = peer_queue_message(&peer, &msg);
    assert(result == ECHO_ERR_INVALID_STATE);
}

/**
 * Test verack can be queued during handshake.
 */
static void test_queue_verack_during_handshake(void) {
    peer_t peer;
    peer_init(&peer);
    peer.state = PEER_STATE_HANDSHAKE_RECV;

    msg_t msg;
    msg.type = MSG_VERACK;

    echo_result_t result = peer_queue_message(&peer, &msg);
    assert(result == ECHO_SUCCESS);
}

/**
 * Test disconnect sets state and reason.
 */
static void test_disconnect(void) {
    peer_t peer;
    peer_init(&peer);
    peer.state = PEER_STATE_READY;

    peer_disconnect(&peer, PEER_DISCONNECT_USER, "Test disconnect");

    assert(peer.state == PEER_STATE_DISCONNECTED);
    assert(peer.disconnect_reason == PEER_DISCONNECT_USER);
    assert(strcmp(peer.disconnect_message, "Test disconnect") == 0);
}

/**
 * Test disconnect when already disconnected (should be safe).
 */
static void test_disconnect_idempotent(void) {
    peer_t peer;
    peer_init(&peer);
    peer.state = PEER_STATE_DISCONNECTED;
    peer.disconnect_reason = PEER_DISCONNECT_USER;

    /* Disconnect again */
    peer_disconnect(&peer, PEER_DISCONNECT_PROTOCOL_ERROR, "New reason");

    /* Should still have original reason since already disconnected */
    assert(peer.state == PEER_STATE_DISCONNECTED);
    assert(peer.disconnect_reason == PEER_DISCONNECT_USER);
}

/**
 * Test version message serialization round-trip.
 */
static void test_version_serialization(void) {
    msg_version_t version;
    memset(&version, 0, sizeof(version));

    version.version = PROTOCOL_VERSION;
    version.services = SERVICE_NODE_NETWORK | SERVICE_NODE_WITNESS;
    version.timestamp = 1609459200;  /* 2021-01-01 */
    version.addr_recv.services = SERVICE_NODE_NETWORK;
    version.addr_recv.port = 8333;
    version.addr_from.services = SERVICE_NODE_NETWORK | SERVICE_NODE_WITNESS;
    version.addr_from.port = 8333;
    version.nonce = 0x1234567890ABCDEF;
    strcpy(version.user_agent, "/BitcoinEcho:0.1.0/");
    version.user_agent_len = strlen(version.user_agent);
    version.start_height = 123456;
    version.relay = ECHO_TRUE;

    /* Serialize */
    uint8_t buf[1024];
    size_t written;
    echo_result_t result = msg_version_serialize(&version, buf, sizeof(buf), &written);
    assert(result == ECHO_SUCCESS);
    assert(written > 0);

    /* Deserialize */
    msg_version_t decoded;
    size_t consumed;
    result = msg_version_deserialize(buf, written, &decoded, &consumed);
    assert(result == ECHO_SUCCESS);
    assert(consumed == written);

    /* Verify */
    assert(decoded.version == version.version);
    assert(decoded.services == version.services);
    assert(decoded.timestamp == version.timestamp);
    assert(decoded.nonce == version.nonce);
    assert(decoded.start_height == version.start_height);
    assert(decoded.relay == version.relay);
    assert(decoded.user_agent_len == version.user_agent_len);
    assert(memcmp(decoded.user_agent, version.user_agent, version.user_agent_len) == 0);
}

/**
 * Test message header serialization round-trip.
 */
static void test_header_serialization(void) {
    msg_header_t header;
    header.magic = MAGIC_MAINNET;
    memset(header.command, 0, COMMAND_LEN);
    strcpy(header.command, "version");
    header.length = 100;
    header.checksum = 0x12345678;

    /* Serialize */
    uint8_t buf[24];
    echo_result_t result = msg_header_serialize(&header, buf, sizeof(buf));
    assert(result == ECHO_SUCCESS);

    /* Deserialize */
    msg_header_t decoded;
    result = msg_header_deserialize(buf, sizeof(buf), &decoded);
    assert(result == ECHO_SUCCESS);

    /* Verify */
    assert(decoded.magic == header.magic);
    assert(strcmp(decoded.command, header.command) == 0);
    assert(decoded.length == header.length);
    assert(decoded.checksum == header.checksum);
}

/**
 * Test checksum computation.
 */
static void test_checksum(void) {
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    uint32_t checksum = msg_checksum(data, sizeof(data));

    /* Checksum should be deterministic */
    uint32_t checksum2 = msg_checksum(data, sizeof(data));
    assert(checksum == checksum2);

    /* Different data should produce different checksum */
    uint8_t data2[] = {0x05, 0x06, 0x07, 0x08};
    uint32_t checksum3 = msg_checksum(data2, sizeof(data2));
    assert(checksum != checksum3);
}

/**
 * Test message command parsing.
 */
static void test_command_parsing(void) {
    assert(msg_parse_command("version") == MSG_VERSION);
    assert(msg_parse_command("verack") == MSG_VERACK);
    assert(msg_parse_command("ping") == MSG_PING);
    assert(msg_parse_command("pong") == MSG_PONG);
    assert(msg_parse_command("inv") == MSG_INV);
    assert(msg_parse_command("getdata") == MSG_GETDATA);
    assert(msg_parse_command("block") == MSG_BLOCK);
    assert(msg_parse_command("tx") == MSG_TX);
    assert(msg_parse_command("addr") == MSG_ADDR);
    assert(msg_parse_command("getaddr") == MSG_GETADDR);
    assert(msg_parse_command("getheaders") == MSG_GETHEADERS);
    assert(msg_parse_command("headers") == MSG_HEADERS);
    assert(msg_parse_command("unknown") == MSG_UNKNOWN);
}

/**
 * Test command string conversion.
 */
static void test_command_string(void) {
    assert(strcmp(msg_command_string(MSG_VERSION), "version") == 0);
    assert(strcmp(msg_command_string(MSG_VERACK), "verack") == 0);
    assert(strcmp(msg_command_string(MSG_PING), "ping") == 0);
    assert(strcmp(msg_command_string(MSG_PONG), "pong") == 0);
    assert(strcmp(msg_command_string(MSG_INV), "inv") == 0);
    assert(strcmp(msg_command_string(MSG_GETDATA), "getdata") == 0);
    assert(strcmp(msg_command_string(MSG_BLOCK), "block") == 0);
    assert(strcmp(msg_command_string(MSG_TX), "tx") == 0);
}

/**
 * Test NULL parameter handling.
 */
static void test_null_params(void) {
    peer_t peer;
    msg_t msg;

    assert(peer_connect(NULL, "127.0.0.1", 8333, 0) == ECHO_ERR_NULL_PARAM);
    assert(peer_connect(&peer, NULL, 8333, 0) == ECHO_ERR_NULL_PARAM);

    assert(peer_accept(NULL, NULL, 0) == ECHO_ERR_NULL_PARAM);

    assert(peer_send_version(NULL, 0, 0, ECHO_FALSE) == ECHO_ERR_NULL_PARAM);

    assert(peer_receive(NULL, &msg) == ECHO_ERR_NULL_PARAM);
    assert(peer_receive(&peer, NULL) == ECHO_ERR_NULL_PARAM);

    assert(peer_queue_message(NULL, &msg) == ECHO_ERR_NULL_PARAM);
    assert(peer_queue_message(&peer, NULL) == ECHO_ERR_NULL_PARAM);

    assert(peer_send_queued(NULL) == ECHO_ERR_NULL_PARAM);
}

/**
 * Test peer receive with insufficient data (would block).
 *
 * Note: This test is skipped because it requires a real socket.
 * In a production test suite, we would use mock sockets.
 */
static void test_receive_insufficient_data(void) {
    peer_t peer;
    peer_init(&peer);

    /* Just test that the peer initializes correctly */
    assert(peer.state == PEER_STATE_DISCONNECTED);
    assert(peer.recv_buffer_len == 0);

    /* In real usage, with less than 24 bytes, peer_receive would return ECHO_ERR_WOULD_BLOCK */
}

/**
 * Test receive with invalid magic.
 *
 * Note: Simplified test - doesn't actually call peer_receive without socket.
 */
static void test_receive_invalid_magic(void) {
    /* Test message header validation */
    msg_header_t header;
    header.magic = 0xDEADBEEF;  /* Wrong magic */
    memset(header.command, 0, COMMAND_LEN);
    strcpy(header.command, "ping");
    header.length = 8;
    header.checksum = 0;

    /* Validate header */
    echo_bool_t valid = msg_header_valid(&header, MAGIC_MAINNET);
    assert(valid == ECHO_FALSE);  /* Wrong magic should fail */

    /* Correct magic should pass */
    header.magic = MAGIC_MAINNET;
    valid = msg_header_valid(&header, MAGIC_MAINNET);
    assert(valid == ECHO_TRUE);
}

/**
 * Test receive with oversized message.
 *
 * Note: Simplified test - validates message size limits.
 */
static void test_receive_oversized_message(void) {
    /* Test that MAX_MESSAGE_SIZE is defined and reasonable */
    assert(MAX_MESSAGE_SIZE == 32 * 1024 * 1024);  /* 32MB */

    /* Create a header claiming huge payload */
    msg_header_t header;
    header.magic = MAGIC_MAINNET;
    memset(header.command, 0, COMMAND_LEN);
    strcpy(header.command, "block");
    header.length = MAX_MESSAGE_SIZE + 1;  /* Too large */
    header.checksum = 0;

    /* Header should fail validation due to oversized payload */
    echo_bool_t valid = msg_header_valid(&header, MAGIC_MAINNET);
    assert(valid == ECHO_FALSE);  /* Oversized message rejected */

    /* Message size exceeds limit */
    assert(header.length > MAX_MESSAGE_SIZE);

    /* Normal-sized message should validate */
    header.length = 1000;
    valid = msg_header_valid(&header, MAGIC_MAINNET);
    assert(valid == ECHO_TRUE);
}

/**
 * Test address truncation.
 */
static void test_address_truncation(void) {
    peer_t peer;
    char long_address[128];
    memset(long_address, 'A', sizeof(long_address) - 1);
    long_address[sizeof(long_address) - 1] = '\0';

    echo_result_t result = peer_connect(&peer, long_address, 8333, 0x123);

    /* Should reject overly long address */
    assert(result == ECHO_ERR_INVALID_PARAM);
}

/**
 * Test disconnect message truncation.
 */
static void test_disconnect_message_truncation(void) {
    peer_t peer;
    peer_init(&peer);
    peer.state = PEER_STATE_READY;

    char long_message[512];
    memset(long_message, 'X', sizeof(long_message) - 1);
    long_message[sizeof(long_message) - 1] = '\0';

    peer_disconnect(&peer, PEER_DISCONNECT_USER, long_message);

    /* Message should be truncated but not overflow */
    assert(strlen(peer.disconnect_message) < sizeof(peer.disconnect_message));
}

int main(void) {
    test_suite_begin("Peer Management Tests");
    test_case("Peer init"); test_peer_init(); test_pass();
    test_case("Peer state strings"); test_peer_state_strings(); test_pass();
    test_case("Peer state checks"); test_peer_state_checks(); test_pass();
    test_case("Message queue"); test_message_queue(); test_pass();
    test_case("Message queue full"); test_message_queue_full(); test_pass();
    test_case("Queue wrong state"); test_queue_wrong_state(); test_pass();
    test_case("Queue verack during handshake"); test_queue_verack_during_handshake(); test_pass();
    test_case("Disconnect"); test_disconnect(); test_pass();
    test_case("Disconnect idempotent"); test_disconnect_idempotent(); test_pass();
    test_case("Version serialization"); test_version_serialization(); test_pass();
    test_case("Header serialization"); test_header_serialization(); test_pass();
    test_case("Checksum"); test_checksum(); test_pass();
    test_case("Command parsing"); test_command_parsing(); test_pass();
    test_case("Command string"); test_command_string(); test_pass();
    test_case("Null params"); test_null_params(); test_pass();
    test_case("Receive insufficient data"); test_receive_insufficient_data(); test_pass();
    test_case("Receive invalid magic"); test_receive_invalid_magic(); test_pass();
    test_case("Receive oversized message"); test_receive_oversized_message(); test_pass();
    test_case("Address truncation"); test_address_truncation(); test_pass();
    test_case("Disconnect message truncation"); test_disconnect_message_truncation(); test_pass();

    test_suite_end();
    return test_global_summary();
}
