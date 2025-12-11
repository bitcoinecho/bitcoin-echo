/**
 * Bitcoin Echo — Protocol Message Serialization
 *
 * Functions for encoding and decoding P2P protocol messages to/from wire format.
 *
 * All serialization follows Bitcoin protocol specification:
 * - Little-endian for all multi-byte integers except port numbers
 * - Port numbers are big-endian (network byte order)
 * - Variable-length fields use CompactSize encoding
 * - IP addresses are 16-byte IPv6 format (IPv4 mapped to ::ffff:x.x.x.x)
 *
 * Message serialization is in two parts:
 * 1. Header (24 bytes) - magic, command, length, checksum
 * 2. Payload (variable) - message-specific data
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_PROTOCOL_SERIALIZE_H
#define ECHO_PROTOCOL_SERIALIZE_H

#include "protocol.h"
#include "echo_types.h"

/**
 * Serialize message header to wire format.
 *
 * Parameters:
 *   header  - Message header to serialize
 *   buf     - Output buffer (must be at least 24 bytes)
 *   buf_len - Size of output buffer
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if header or buf is NULL
 *   ECHO_ERR_BUFFER_TOO_SMALL if buf_len < 24
 */
echo_result_t msg_header_serialize(const msg_header_t *header, uint8_t *buf,
                                    size_t buf_len);

/**
 * Deserialize message header from wire format.
 *
 * Parameters:
 *   buf    - Input buffer (at least 24 bytes)
 *   buf_len - Size of input buffer
 *   header - Output: parsed message header
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if buf or header is NULL
 *   ECHO_ERR_TRUNCATED if buf_len < 24
 */
echo_result_t msg_header_deserialize(const uint8_t *buf, size_t buf_len,
                                      msg_header_t *header);

/**
 * Serialize version message to wire format.
 *
 * Parameters:
 *   msg     - Version message to serialize
 *   buf     - Output buffer
 *   buf_len - Size of output buffer
 *   written - Output: number of bytes written
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if msg, buf, or written is NULL
 *   ECHO_ERR_BUFFER_TOO_SMALL if buffer is insufficient
 */
echo_result_t msg_version_serialize(const msg_version_t *msg, uint8_t *buf,
                                     size_t buf_len, size_t *written);

/**
 * Deserialize version message from wire format.
 *
 * Parameters:
 *   buf      - Input buffer
 *   buf_len  - Size of input buffer
 *   msg      - Output: parsed version message
 *   consumed - Output: number of bytes consumed (may be NULL)
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if buf or msg is NULL
 *   ECHO_ERR_TRUNCATED if buffer is too short
 *   ECHO_ERR_INVALID_FORMAT if data is malformed
 */
echo_result_t msg_version_deserialize(const uint8_t *buf, size_t buf_len,
                                       msg_version_t *msg, size_t *consumed);

/**
 * Serialize inv/getdata/notfound message to wire format.
 */
echo_result_t msg_inv_serialize(const msg_inv_t *msg, uint8_t *buf,
                                size_t buf_len, size_t *written);

/**
 * Deserialize inv/getdata/notfound message from wire format.
 */
echo_result_t msg_inv_deserialize(const uint8_t *buf, size_t buf_len,
                                  msg_inv_t *msg, size_t *consumed);

/**
 * Serialize addr message to wire format.
 */
echo_result_t msg_addr_serialize(const msg_addr_t *msg, uint8_t *buf,
                                 size_t buf_len, size_t *written);

/**
 * Deserialize addr message from wire format.
 */
echo_result_t msg_addr_deserialize(const uint8_t *buf, size_t buf_len,
                                   msg_addr_t *msg, size_t *consumed);

/**
 * Serialize getheaders/getblocks message to wire format.
 */
echo_result_t msg_getheaders_serialize(const msg_getheaders_t *msg, uint8_t *buf,
                                       size_t buf_len, size_t *written);

/**
 * Deserialize getheaders/getblocks message from wire format.
 */
echo_result_t msg_getheaders_deserialize(const uint8_t *buf, size_t buf_len,
                                         msg_getheaders_t *msg, size_t *consumed);

/**
 * Serialize headers message to wire format.
 */
echo_result_t msg_headers_serialize(const msg_headers_t *msg, uint8_t *buf,
                                    size_t buf_len, size_t *written);

/**
 * Deserialize headers message from wire format.
 */
echo_result_t msg_headers_deserialize(const uint8_t *buf, size_t buf_len,
                                      msg_headers_t *msg, size_t *consumed);

/**
 * Serialize ping message to wire format.
 */
echo_result_t msg_ping_serialize(const msg_ping_t *msg, uint8_t *buf,
                                 size_t buf_len, size_t *written);

/**
 * Deserialize ping message from wire format.
 */
echo_result_t msg_ping_deserialize(const uint8_t *buf, size_t buf_len,
                                   msg_ping_t *msg, size_t *consumed);

/**
 * Serialize pong message to wire format (same as ping).
 */
echo_result_t msg_pong_serialize(const msg_pong_t *msg, uint8_t *buf,
                                 size_t buf_len, size_t *written);

/**
 * Deserialize pong message from wire format (same as ping).
 */
echo_result_t msg_pong_deserialize(const uint8_t *buf, size_t buf_len,
                                   msg_pong_t *msg, size_t *consumed);

/**
 * Serialize block message to wire format.
 */
echo_result_t msg_block_serialize(const msg_block_t *msg, uint8_t *buf,
                                  size_t buf_len, size_t *written);

/**
 * Deserialize block message from wire format.
 */
echo_result_t msg_block_deserialize(const uint8_t *buf, size_t buf_len,
                                    msg_block_t *msg, size_t *consumed);

/**
 * Serialize tx message to wire format.
 */
echo_result_t msg_tx_serialize(const msg_tx_t *msg, uint8_t *buf,
                               size_t buf_len, size_t *written);

/**
 * Deserialize tx message from wire format.
 */
echo_result_t msg_tx_deserialize(const uint8_t *buf, size_t buf_len,
                                 msg_tx_t *msg, size_t *consumed);

/**
 * Serialize reject message to wire format.
 */
echo_result_t msg_reject_serialize(const msg_reject_t *msg, uint8_t *buf,
                                   size_t buf_len, size_t *written);

/**
 * Deserialize reject message from wire format.
 */
echo_result_t msg_reject_deserialize(const uint8_t *buf, size_t buf_len,
                                     msg_reject_t *msg, size_t *consumed);

/**
 * Serialize feefilter message to wire format.
 */
echo_result_t msg_feefilter_serialize(const msg_feefilter_t *msg, uint8_t *buf,
                                      size_t buf_len, size_t *written);

/**
 * Deserialize feefilter message from wire format.
 */
echo_result_t msg_feefilter_deserialize(const uint8_t *buf, size_t buf_len,
                                        msg_feefilter_t *msg, size_t *consumed);

/**
 * Serialize sendcmpct message to wire format.
 */
echo_result_t msg_sendcmpct_serialize(const msg_sendcmpct_t *msg, uint8_t *buf,
                                      size_t buf_len, size_t *written);

/**
 * Deserialize sendcmpct message from wire format.
 */
echo_result_t msg_sendcmpct_deserialize(const uint8_t *buf, size_t buf_len,
                                        msg_sendcmpct_t *msg, size_t *consumed);

/* Helper functions for serializing common protocol primitives */

/**
 * Write a uint8_t to buffer.
 */
echo_result_t write_u8(uint8_t **ptr, const uint8_t *end, uint8_t val);

/**
 * Write a uint16_t to buffer (little-endian).
 */
echo_result_t write_u16_le(uint8_t **ptr, const uint8_t *end, uint16_t val);

/**
 * Write a uint16_t to buffer (big-endian, for port numbers).
 */
echo_result_t write_u16_be(uint8_t **ptr, const uint8_t *end, uint16_t val);

/**
 * Write a uint32_t to buffer (little-endian).
 */
echo_result_t write_u32_le(uint8_t **ptr, const uint8_t *end, uint32_t val);

/**
 * Write a uint64_t to buffer (little-endian).
 */
echo_result_t write_u64_le(uint8_t **ptr, const uint8_t *end, uint64_t val);

/**
 * Write a int32_t to buffer (little-endian).
 */
echo_result_t write_i32_le(uint8_t **ptr, const uint8_t *end, int32_t val);

/**
 * Write a int64_t to buffer (little-endian).
 */
echo_result_t write_i64_le(uint8_t **ptr, const uint8_t *end, int64_t val);

/**
 * Write raw bytes to buffer.
 */
echo_result_t write_bytes(uint8_t **ptr, const uint8_t *end, const uint8_t *data, size_t len);

/**
 * Read a uint8_t from buffer.
 */
echo_result_t read_u8(const uint8_t **ptr, const uint8_t *end, uint8_t *val);

/**
 * Read a uint16_t from buffer (little-endian).
 */
echo_result_t read_u16_le(const uint8_t **ptr, const uint8_t *end, uint16_t *val);

/**
 * Read a uint16_t from buffer (big-endian, for port numbers).
 */
echo_result_t read_u16_be(const uint8_t **ptr, const uint8_t *end, uint16_t *val);

/**
 * Read a uint32_t from buffer (little-endian).
 */
echo_result_t read_u32_le(const uint8_t **ptr, const uint8_t *end, uint32_t *val);

/**
 * Read a uint64_t from buffer (little-endian).
 */
echo_result_t read_u64_le(const uint8_t **ptr, const uint8_t *end, uint64_t *val);

/**
 * Read a int32_t from buffer (little-endian).
 */
echo_result_t read_i32_le(const uint8_t **ptr, const uint8_t *end, int32_t *val);

/**
 * Read a int64_t from buffer (little-endian).
 */
echo_result_t read_i64_le(const uint8_t **ptr, const uint8_t *end, int64_t *val);

/**
 * Read raw bytes from buffer.
 */
echo_result_t read_bytes(const uint8_t **ptr, const uint8_t *end, uint8_t *data, size_t len);

#endif /* ECHO_PROTOCOL_SERIALIZE_H */
