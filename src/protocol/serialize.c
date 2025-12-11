/**
 * Bitcoin Echo — Protocol Message Serialization
 *
 * Implementation of protocol message serialization to/from wire format.
 *
 * All serialization follows Bitcoin protocol specification with little-endian
 * encoding for multi-byte integers (except port numbers which are big-endian).
 */

#include "serialize.h"
#include "block.h"
#include "echo_types.h"
#include "protocol.h"
#include "protocol_serialize.h"
#include "tx.h"
#include <_string.h>
#include <stdint.h>
#include <string.h>

/* ========================================================================
 * Helper Functions for Reading/Writing Primitive Types
 * ======================================================================== */

echo_result_t write_u8(uint8_t **ptr, const uint8_t *end, uint8_t val) {
  if (*ptr >= end) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }
  **ptr = val;
  (*ptr)++;
  return ECHO_OK;
}

echo_result_t write_u16_le(uint8_t **ptr, const uint8_t *end, uint16_t val) {
  if (*ptr + 2 > end) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }
  (*ptr)[0] = (uint8_t)(val & 0xFF);
  (*ptr)[1] = (uint8_t)((val >> 8) & 0xFF);
  *ptr += 2;
  return ECHO_OK;
}

echo_result_t write_u16_be(uint8_t **ptr, const uint8_t *end, uint16_t val) {
  if (*ptr + 2 > end) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }
  (*ptr)[0] = (uint8_t)((val >> 8) & 0xFF);
  (*ptr)[1] = (uint8_t)(val & 0xFF);
  *ptr += 2;
  return ECHO_OK;
}

echo_result_t write_u32_le(uint8_t **ptr, const uint8_t *end, uint32_t val) {
  if (*ptr + 4 > end) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }
  (*ptr)[0] = (uint8_t)(val & 0xFF);
  (*ptr)[1] = (uint8_t)((val >> 8) & 0xFF);
  (*ptr)[2] = (uint8_t)((val >> 16) & 0xFF);
  (*ptr)[3] = (uint8_t)((val >> 24) & 0xFF);
  *ptr += 4;
  return ECHO_OK;
}

echo_result_t write_u64_le(uint8_t **ptr, const uint8_t *end, uint64_t val) {
  if (*ptr + 8 > end) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }
  (*ptr)[0] = (uint8_t)(val & 0xFF);
  (*ptr)[1] = (uint8_t)((val >> 8) & 0xFF);
  (*ptr)[2] = (uint8_t)((val >> 16) & 0xFF);
  (*ptr)[3] = (uint8_t)((val >> 24) & 0xFF);
  (*ptr)[4] = (uint8_t)((val >> 32) & 0xFF);
  (*ptr)[5] = (uint8_t)((val >> 40) & 0xFF);
  (*ptr)[6] = (uint8_t)((val >> 48) & 0xFF);
  (*ptr)[7] = (uint8_t)((val >> 56) & 0xFF);
  *ptr += 8;
  return ECHO_OK;
}

echo_result_t write_i32_le(uint8_t **ptr, const uint8_t *end, int32_t val) {
  return write_u32_le(ptr, end, (uint32_t)val);
}

echo_result_t write_i64_le(uint8_t **ptr, const uint8_t *end, int64_t val) {
  return write_u64_le(ptr, end, (uint64_t)val);
}

echo_result_t write_bytes(uint8_t **ptr, const uint8_t *end,
                          const uint8_t *data, size_t len) {
  if (*ptr + len > end) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }
  memcpy(*ptr, data, len);
  *ptr += len;
  return ECHO_OK;
}

echo_result_t read_u8(const uint8_t **ptr, const uint8_t *end, uint8_t *val) {
  if (*ptr >= end) {
    return ECHO_ERR_TRUNCATED;
  }
  *val = **ptr;
  (*ptr)++;
  return ECHO_OK;
}

echo_result_t read_u16_le(const uint8_t **ptr, const uint8_t *end,
                          uint16_t *val) {
  if (*ptr + 2 > end) {
    return ECHO_ERR_TRUNCATED;
  }
  *val = (uint16_t)((uint16_t)((*ptr)[0]) | ((uint16_t)((*ptr)[1]) << 8));
  *ptr += 2;
  return ECHO_OK;
}

echo_result_t read_u16_be(const uint8_t **ptr, const uint8_t *end,
                          uint16_t *val) {
  if (*ptr + 2 > end) {
    return ECHO_ERR_TRUNCATED;
  }
  *val = (uint16_t)(((uint16_t)((*ptr)[0]) << 8) | (uint16_t)((*ptr)[1]));
  *ptr += 2;
  return ECHO_OK;
}

echo_result_t read_u32_le(const uint8_t **ptr, const uint8_t *end,
                          uint32_t *val) {
  if (*ptr + 4 > end) {
    return ECHO_ERR_TRUNCATED;
  }
  *val = (uint32_t)((*ptr)[0]) | ((uint32_t)((*ptr)[1]) << 8) |
         ((uint32_t)((*ptr)[2]) << 16) | ((uint32_t)((*ptr)[3]) << 24);
  *ptr += 4;
  return ECHO_OK;
}

echo_result_t read_u64_le(const uint8_t **ptr, const uint8_t *end,
                          uint64_t *val) {
  if (*ptr + 8 > end) {
    return ECHO_ERR_TRUNCATED;
  }
  *val = (uint64_t)((*ptr)[0]) | ((uint64_t)((*ptr)[1]) << 8) |
         ((uint64_t)((*ptr)[2]) << 16) | ((uint64_t)((*ptr)[3]) << 24) |
         ((uint64_t)((*ptr)[4]) << 32) | ((uint64_t)((*ptr)[5]) << 40) |
         ((uint64_t)((*ptr)[6]) << 48) | ((uint64_t)((*ptr)[7]) << 56);
  *ptr += 8;
  return ECHO_OK;
}

echo_result_t read_i32_le(const uint8_t **ptr, const uint8_t *end,
                          int32_t *val) {
  uint32_t u;
  echo_result_t res = read_u32_le(ptr, end, &u);
  if (res == ECHO_OK) {
    *val = (int32_t)u;
  }
  return res;
}

echo_result_t read_i64_le(const uint8_t **ptr, const uint8_t *end,
                          int64_t *val) {
  uint64_t u;
  echo_result_t res = read_u64_le(ptr, end, &u);
  if (res == ECHO_OK) {
    *val = (int64_t)u;
  }
  return res;
}

echo_result_t read_bytes(const uint8_t **ptr, const uint8_t *end, uint8_t *data,
                         size_t len) {
  if (*ptr + len > end) {
    return ECHO_ERR_TRUNCATED;
  }
  memcpy(data, *ptr, len);
  *ptr += len;
  return ECHO_OK;
}

/* ========================================================================
 * Message Header Serialization
 * ======================================================================== */

echo_result_t msg_header_serialize(const msg_header_t *header, uint8_t *buf,
                                   size_t buf_len) {
  if (!header || !buf) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (buf_len < 24) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }

  uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;

  /* Write magic (4 bytes, little-endian) */
  echo_result_t res = write_u32_le(&ptr, end, header->magic);
  if (res != ECHO_OK)
    return res;

  /* Write command (12 bytes, null-padded) */
  res = write_bytes(&ptr, end, (const uint8_t *)header->command, COMMAND_LEN);
  if (res != ECHO_OK)
    return res;

  /* Write length (4 bytes, little-endian) */
  res = write_u32_le(&ptr, end, header->length);
  if (res != ECHO_OK)
    return res;

  /* Write checksum (4 bytes, little-endian) */
  res = write_u32_le(&ptr, end, header->checksum);
  if (res != ECHO_OK)
    return res;

  return ECHO_OK;
}

echo_result_t msg_header_deserialize(const uint8_t *buf, size_t buf_len,
                                     msg_header_t *header) {
  if (!buf || !header) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (buf_len < 24) {
    return ECHO_ERR_TRUNCATED;
  }

  const uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;

  /* Read magic */
  echo_result_t res = read_u32_le(&ptr, end, &header->magic);
  if (res != ECHO_OK)
    return res;

  /* Read command */
  res = read_bytes(&ptr, end, (uint8_t *)header->command, COMMAND_LEN);
  if (res != ECHO_OK)
    return res;

  /* Read length */
  res = read_u32_le(&ptr, end, &header->length);
  if (res != ECHO_OK)
    return res;

  /* Read checksum */
  res = read_u32_le(&ptr, end, &header->checksum);
  if (res != ECHO_OK)
    return res;

  return ECHO_OK;
}

/* ========================================================================
 * Network Address Serialization (Helper)
 * ======================================================================== */

static echo_result_t write_net_addr_notime(uint8_t **ptr, const uint8_t *end,
                                           const net_addr_notime_t *addr) {
  echo_result_t res;

  /* Services (8 bytes, little-endian) */
  res = write_u64_le(ptr, end, addr->services);
  if (res != ECHO_OK)
    return res;

  /* IP address (16 bytes) */
  res = write_bytes(ptr, end, addr->ip, 16);
  if (res != ECHO_OK)
    return res;

  /* Port (2 bytes, big-endian) */
  res = write_u16_be(ptr, end, addr->port);
  if (res != ECHO_OK)
    return res;

  return ECHO_OK;
}

static echo_result_t read_net_addr_notime(const uint8_t **ptr,
                                          const uint8_t *end,
                                          net_addr_notime_t *addr) {
  echo_result_t res;

  /* Services */
  res = read_u64_le(ptr, end, &addr->services);
  if (res != ECHO_OK)
    return res;

  /* IP address */
  res = read_bytes(ptr, end, addr->ip, 16);
  if (res != ECHO_OK)
    return res;

  /* Port (big-endian) */
  res = read_u16_be(ptr, end, &addr->port);
  if (res != ECHO_OK)
    return res;

  return ECHO_OK;
}

static echo_result_t write_net_addr(uint8_t **ptr, const uint8_t *end,
                                    const net_addr_t *addr) {
  echo_result_t res;

  /* Timestamp (4 bytes, little-endian) */
  res = write_u32_le(ptr, end, addr->timestamp);
  if (res != ECHO_OK)
    return res;

  /* Services (8 bytes, little-endian) */
  res = write_u64_le(ptr, end, addr->services);
  if (res != ECHO_OK)
    return res;

  /* IP address (16 bytes) */
  res = write_bytes(ptr, end, addr->ip, 16);
  if (res != ECHO_OK)
    return res;

  /* Port (2 bytes, big-endian) */
  res = write_u16_be(ptr, end, addr->port);
  if (res != ECHO_OK)
    return res;

  return ECHO_OK;
}

static echo_result_t read_net_addr(const uint8_t **ptr, const uint8_t *end,
                                   net_addr_t *addr) {
  echo_result_t res;

  /* Timestamp */
  res = read_u32_le(ptr, end, &addr->timestamp);
  if (res != ECHO_OK)
    return res;

  /* Services */
  res = read_u64_le(ptr, end, &addr->services);
  if (res != ECHO_OK)
    return res;

  /* IP address */
  res = read_bytes(ptr, end, addr->ip, 16);
  if (res != ECHO_OK)
    return res;

  /* Port (big-endian) */
  res = read_u16_be(ptr, end, &addr->port);
  if (res != ECHO_OK)
    return res;

  return ECHO_OK;
}

/* ========================================================================
 * version Message Serialization
 * ======================================================================== */

echo_result_t msg_version_serialize(const msg_version_t *msg, uint8_t *buf,
                                    size_t buf_len, size_t *written) {
  if (!msg || !buf || !written) {
    return ECHO_ERR_NULL_PARAM;
  }

  uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;
  echo_result_t res;

  /* Version (4 bytes) */
  res = write_i32_le(&ptr, end, msg->version);
  if (res != ECHO_OK)
    return res;

  /* Services (8 bytes) */
  res = write_u64_le(&ptr, end, msg->services);
  if (res != ECHO_OK)
    return res;

  /* Timestamp (8 bytes) */
  res = write_i64_le(&ptr, end, msg->timestamp);
  if (res != ECHO_OK)
    return res;

  /* Receiver address (26 bytes) */
  res = write_net_addr_notime(&ptr, end, &msg->addr_recv);
  if (res != ECHO_OK)
    return res;

  /* Sender address (26 bytes) */
  res = write_net_addr_notime(&ptr, end, &msg->addr_from);
  if (res != ECHO_OK)
    return res;

  /* Nonce (8 bytes) */
  res = write_u64_le(&ptr, end, msg->nonce);
  if (res != ECHO_OK)
    return res;

  /* User agent (varint + string) */
  size_t varint_len;
  res =
      varint_write(ptr, (size_t)(end - ptr), msg->user_agent_len, &varint_len);
  if (res != ECHO_OK)
    return res;
  ptr += varint_len;

  res = write_bytes(&ptr, end, (const uint8_t *)msg->user_agent,
                    msg->user_agent_len);
  if (res != ECHO_OK)
    return res;

  /* Start height (4 bytes) */
  res = write_i32_le(&ptr, end, msg->start_height);
  if (res != ECHO_OK)
    return res;

  /* Relay flag (1 byte) - only if protocol version >= 70001 */
  if (msg->version >= 70001) {
    res = write_u8(&ptr, end, msg->relay ? 1 : 0);
    if (res != ECHO_OK)
      return res;
  }

  *written = (size_t)(ptr - buf);
  return ECHO_OK;
}

echo_result_t msg_version_deserialize(const uint8_t *buf, size_t buf_len,
                                      msg_version_t *msg, size_t *consumed) {
  if (!buf || !msg) {
    return ECHO_ERR_NULL_PARAM;
  }

  const uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;
  echo_result_t res;

  /* Version */
  res = read_i32_le(&ptr, end, &msg->version);
  if (res != ECHO_OK)
    return res;

  /* Services */
  res = read_u64_le(&ptr, end, &msg->services);
  if (res != ECHO_OK)
    return res;

  /* Timestamp */
  res = read_i64_le(&ptr, end, &msg->timestamp);
  if (res != ECHO_OK)
    return res;

  /* Receiver address */
  res = read_net_addr_notime(&ptr, end, &msg->addr_recv);
  if (res != ECHO_OK)
    return res;

  /* Sender address */
  res = read_net_addr_notime(&ptr, end, &msg->addr_from);
  if (res != ECHO_OK)
    return res;

  /* Nonce */
  res = read_u64_le(&ptr, end, &msg->nonce);
  if (res != ECHO_OK)
    return res;

  /* User agent */
  uint64_t ua_len;
  size_t varint_len;
  res = varint_read(ptr, (size_t)(end - ptr), &ua_len, &varint_len);
  if (res != ECHO_OK)
    return res;
  ptr += varint_len;

  if (ua_len > MAX_USER_AGENT_LEN) {
    return ECHO_ERR_INVALID_FORMAT;
  }

  msg->user_agent_len = (size_t)ua_len;
  res = read_bytes(&ptr, end, (uint8_t *)msg->user_agent, msg->user_agent_len);
  if (res != ECHO_OK)
    return res;

  /* Null-terminate user agent */
  if (msg->user_agent_len < MAX_USER_AGENT_LEN) {
    msg->user_agent[msg->user_agent_len] = '\0';
  } else {
    msg->user_agent[MAX_USER_AGENT_LEN - 1] = '\0';
  }

  /* Start height */
  res = read_i32_le(&ptr, end, &msg->start_height);
  if (res != ECHO_OK)
    return res;

  /* Relay flag (optional, only in version >= 70001) */
  if (msg->version >= 70001 && ptr < end) {
    uint8_t relay_byte;
    res = read_u8(&ptr, end, &relay_byte);
    if (res != ECHO_OK)
      return res;
    msg->relay = relay_byte ? ECHO_TRUE : ECHO_FALSE;
  } else {
    msg->relay = ECHO_TRUE; /* Default to true for older versions */
  }

  if (consumed) {
    *consumed = (size_t)(ptr - buf);
  }

  return ECHO_OK;
}

/* ========================================================================
 * ping/pong Message Serialization
 * ======================================================================== */

echo_result_t msg_ping_serialize(const msg_ping_t *msg, uint8_t *buf,
                                 size_t buf_len, size_t *written) {
  if (!msg || !buf || !written) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (buf_len < 8) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }

  uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;

  echo_result_t res = write_u64_le(&ptr, end, msg->nonce);
  if (res != ECHO_OK)
    return res;

  *written = 8;
  return ECHO_OK;
}

echo_result_t msg_ping_deserialize(const uint8_t *buf, size_t buf_len,
                                   msg_ping_t *msg, size_t *consumed) {
  if (!buf || !msg) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (buf_len < 8) {
    return ECHO_ERR_TRUNCATED;
  }

  const uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;

  echo_result_t res = read_u64_le(&ptr, end, &msg->nonce);
  if (res != ECHO_OK)
    return res;

  if (consumed) {
    *consumed = 8;
  }

  return ECHO_OK;
}

echo_result_t msg_pong_serialize(const msg_pong_t *msg, uint8_t *buf,
                                 size_t buf_len, size_t *written) {
  return msg_ping_serialize((const msg_ping_t *)msg, buf, buf_len, written);
}

echo_result_t msg_pong_deserialize(const uint8_t *buf, size_t buf_len,
                                   msg_pong_t *msg, size_t *consumed) {
  return msg_ping_deserialize(buf, buf_len, (msg_ping_t *)msg, consumed);
}

/* ========================================================================
 * inv/getdata/notfound Message Serialization
 * ======================================================================== */

echo_result_t msg_inv_serialize(const msg_inv_t *msg, uint8_t *buf,
                                size_t buf_len, size_t *written) {
  if (!msg || !buf || !written) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (msg->count > MAX_INV_ENTRIES) {
    return ECHO_ERR_INVALID_FORMAT;
  }

  uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;
  echo_result_t res;

  /* Write count (varint) */
  size_t varint_len;
  res = varint_write(ptr, (size_t)(end - ptr), msg->count, &varint_len);
  if (res != ECHO_OK)
    return res;
  ptr += varint_len;

  /* Write inventory vectors */
  for (size_t i = 0; i < msg->count; i++) {
    /* Type (4 bytes) */
    res = write_u32_le(&ptr, end, msg->inventory[i].type);
    if (res != ECHO_OK)
      return res;

    /* Hash (32 bytes) */
    res = write_bytes(&ptr, end, msg->inventory[i].hash.bytes, 32);
    if (res != ECHO_OK)
      return res;
  }

  *written = (size_t)(ptr - buf);
  return ECHO_OK;
}

echo_result_t msg_inv_deserialize(const uint8_t *buf, size_t buf_len,
                                  msg_inv_t *msg, size_t *consumed) {
  if (!buf || !msg) {
    return ECHO_ERR_NULL_PARAM;
  }

  const uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;
  echo_result_t res;

  /* Read count */
  uint64_t count;
  size_t varint_len;
  res = varint_read(ptr, (size_t)(end - ptr), &count, &varint_len);
  if (res != ECHO_OK)
    return res;
  ptr += varint_len;

  if (count > MAX_INV_ENTRIES) {
    return ECHO_ERR_INVALID_FORMAT;
  }

  msg->count = (size_t)count;
  msg->inventory = NULL; /* Caller must allocate */

  if (consumed) {
    /* Calculate consumed bytes including all inventory vectors */
    size_t inv_bytes = msg->count * 36; /* 4 bytes type + 32 bytes hash */
    if (ptr + inv_bytes > end) {
      return ECHO_ERR_TRUNCATED;
    }
    *consumed = (size_t)(ptr - buf) + inv_bytes;
  }

  return ECHO_OK;
}

/* ========================================================================
 * addr Message Serialization
 * ======================================================================== */

echo_result_t msg_addr_serialize(const msg_addr_t *msg, uint8_t *buf,
                                 size_t buf_len, size_t *written) {
  if (!msg || !buf || !written) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (msg->count > MAX_ADDR_COUNT) {
    return ECHO_ERR_INVALID_FORMAT;
  }

  uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;
  echo_result_t res;

  /* Write count (varint) */
  size_t varint_len;
  res = varint_write(ptr, (size_t)(end - ptr), msg->count, &varint_len);
  if (res != ECHO_OK)
    return res;
  ptr += varint_len;

  /* Write addresses */
  for (size_t i = 0; i < msg->count; i++) {
    res = write_net_addr(&ptr, end, &msg->addresses[i]);
    if (res != ECHO_OK)
      return res;
  }

  *written = (size_t)(ptr - buf);
  return ECHO_OK;
}

echo_result_t msg_addr_deserialize(const uint8_t *buf, size_t buf_len,
                                   msg_addr_t *msg, size_t *consumed) {
  if (!buf || !msg) {
    return ECHO_ERR_NULL_PARAM;
  }

  const uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;
  echo_result_t res;

  /* Read count */
  uint64_t count;
  size_t varint_len;
  res = varint_read(ptr, (size_t)(end - ptr), &count, &varint_len);
  if (res != ECHO_OK)
    return res;
  ptr += varint_len;

  if (count > MAX_ADDR_COUNT) {
    return ECHO_ERR_INVALID_FORMAT;
  }

  msg->count = (size_t)count;
  msg->addresses = NULL; /* Caller must allocate */

  if (consumed) {
    /* Each address is 30 bytes (4 timestamp + 8 services + 16 ip + 2 port) */
    size_t addr_bytes = msg->count * 30;
    if (ptr + addr_bytes > end) {
      return ECHO_ERR_TRUNCATED;
    }
    *consumed = (size_t)(ptr - buf) + addr_bytes;
  }

  return ECHO_OK;
}

/* ========================================================================
 * getheaders/getblocks Message Serialization
 * ======================================================================== */

echo_result_t msg_getheaders_serialize(const msg_getheaders_t *msg,
                                       uint8_t *buf, size_t buf_len,
                                       size_t *written) {
  if (!msg || !buf || !written) {
    return ECHO_ERR_NULL_PARAM;
  }

  uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;
  echo_result_t res;

  /* Version (4 bytes) */
  res = write_u32_le(&ptr, end, msg->version);
  if (res != ECHO_OK)
    return res;

  /* Hash count (varint) */
  size_t varint_len;
  res = varint_write(ptr, (size_t)(end - ptr), msg->hash_count, &varint_len);
  if (res != ECHO_OK)
    return res;
  ptr += varint_len;

  /* Block locator hashes */
  for (size_t i = 0; i < msg->hash_count; i++) {
    res = write_bytes(&ptr, end, msg->block_locator[i].bytes, 32);
    if (res != ECHO_OK)
      return res;
  }

  /* Hash stop (32 bytes) */
  res = write_bytes(&ptr, end, msg->hash_stop.bytes, 32);
  if (res != ECHO_OK)
    return res;

  *written = (size_t)(ptr - buf);
  return ECHO_OK;
}

echo_result_t msg_getheaders_deserialize(const uint8_t *buf, size_t buf_len,
                                         msg_getheaders_t *msg,
                                         size_t *consumed) {
  if (!buf || !msg) {
    return ECHO_ERR_NULL_PARAM;
  }

  const uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;
  echo_result_t res;

  /* Version */
  res = read_u32_le(&ptr, end, &msg->version);
  if (res != ECHO_OK)
    return res;

  /* Hash count */
  uint64_t count;
  size_t varint_len;
  res = varint_read(ptr, (size_t)(end - ptr), &count, &varint_len);
  if (res != ECHO_OK)
    return res;
  ptr += varint_len;

  msg->hash_count = (size_t)count;
  msg->block_locator = NULL; /* Caller must allocate */

  /* Calculate total size needed */
  size_t hash_bytes = msg->hash_count * 32;
  if (ptr + hash_bytes + 32 > end) {
    return ECHO_ERR_TRUNCATED;
  }

  if (consumed) {
    *consumed = (size_t)(ptr - buf) + hash_bytes + 32;
  }

  return ECHO_OK;
}

/* ========================================================================
 * headers Message Serialization
 * ======================================================================== */

echo_result_t msg_headers_serialize(const msg_headers_t *msg, uint8_t *buf,
                                    size_t buf_len, size_t *written) {
  if (!msg || !buf || !written) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (msg->count > MAX_HEADERS_COUNT) {
    return ECHO_ERR_INVALID_FORMAT;
  }

  uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;
  echo_result_t res;

  /* Count (varint) */
  size_t varint_len;
  res = varint_write(ptr, (size_t)(end - ptr), msg->count, &varint_len);
  if (res != ECHO_OK)
    return res;
  ptr += varint_len;

  /* Headers (each header is 80 bytes + varint 0 for tx count) */
  for (size_t i = 0; i < msg->count; i++) {
    /* Serialize header (80 bytes) */
    res = block_header_serialize(&msg->headers[i], ptr, (size_t)(end - ptr));
    if (res != ECHO_OK)
      return res;
    ptr += 80; /* Header is always 80 bytes */

    /* Transaction count (always 0 for headers message) */
    res = varint_write(ptr, (size_t)(end - ptr), 0, &varint_len);
    if (res != ECHO_OK)
      return res;
    ptr += varint_len;
  }

  *written = (size_t)(ptr - buf);
  return ECHO_OK;
}

echo_result_t msg_headers_deserialize(const uint8_t *buf, size_t buf_len,
                                      msg_headers_t *msg, size_t *consumed) {
  if (!buf || !msg) {
    return ECHO_ERR_NULL_PARAM;
  }

  const uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;
  echo_result_t res;

  /* Count */
  uint64_t count;
  size_t varint_len;
  res = varint_read(ptr, (size_t)(end - ptr), &count, &varint_len);
  if (res != ECHO_OK)
    return res;
  ptr += varint_len;

  if (count > MAX_HEADERS_COUNT) {
    return ECHO_ERR_INVALID_FORMAT;
  }

  msg->count = (size_t)count;
  msg->headers = NULL; /* Caller must allocate */

  if (consumed) {
    /* Each header is 80 bytes + 1 byte varint (0) = 81 bytes */
    size_t headers_bytes = msg->count * 81;
    if (ptr + headers_bytes > end) {
      return ECHO_ERR_TRUNCATED;
    }
    *consumed = (size_t)(ptr - buf) + headers_bytes;
  }

  return ECHO_OK;
}

/* ========================================================================
 * block Message Serialization
 * ======================================================================== */

echo_result_t msg_block_serialize(const msg_block_t *msg, uint8_t *buf,
                                  size_t buf_len, size_t *written) {
  if (!msg || !buf || !written) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Use block serialization from block.c */
  return block_serialize(&msg->block, buf, buf_len, written);
}

echo_result_t msg_block_deserialize(const uint8_t *buf, size_t buf_len,
                                    msg_block_t *msg, size_t *consumed) {
  if (!buf || !msg) {
    return ECHO_ERR_NULL_PARAM;
  }

  (void)buf_len; /* Unused for now */

  /* Block deserialization - to be implemented when needed */
  /* For now, just indicate invalid format */
  if (consumed) {
    *consumed = 0;
  }
  return ECHO_ERR_INVALID_FORMAT;
}

/* ========================================================================
 * tx Message Serialization
 * ======================================================================== */

echo_result_t msg_tx_serialize(const msg_tx_t *msg, uint8_t *buf,
                               size_t buf_len, size_t *written) {
  if (!msg || !buf || !written) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Use transaction serialization from tx.c with witness data */
  return tx_serialize(&msg->tx, ECHO_TRUE, buf, buf_len, written);
}

echo_result_t msg_tx_deserialize(const uint8_t *buf, size_t buf_len,
                                 msg_tx_t *msg, size_t *consumed) {
  if (!buf || !msg) {
    return ECHO_ERR_NULL_PARAM;
  }

  (void)buf_len; /* Unused for now */

  /* Transaction deserialization - to be implemented when needed */
  /* For now, just indicate invalid format */
  if (consumed) {
    *consumed = 0;
  }
  return ECHO_ERR_INVALID_FORMAT;
}

/* ========================================================================
 * reject Message Serialization
 * ======================================================================== */

echo_result_t msg_reject_serialize(const msg_reject_t *msg, uint8_t *buf,
                                   size_t buf_len, size_t *written) {
  if (!msg || !buf || !written) {
    return ECHO_ERR_NULL_PARAM;
  }

  uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;
  echo_result_t res;

  /* Message (varint length + string) */
  size_t msg_len = strnlen(msg->message, COMMAND_LEN);
  size_t varint_len;
  res = varint_write(ptr, (size_t)(end - ptr), msg_len, &varint_len);
  if (res != ECHO_OK)
    return res;
  ptr += varint_len;

  res = write_bytes(&ptr, end, (const uint8_t *)msg->message, msg_len);
  if (res != ECHO_OK)
    return res;

  /* Reject code (1 byte) */
  res = write_u8(&ptr, end, msg->ccode);
  if (res != ECHO_OK)
    return res;

  /* Reason (varint length + string) */
  res = varint_write(ptr, (size_t)(end - ptr), msg->reason_len, &varint_len);
  if (res != ECHO_OK)
    return res;
  ptr += varint_len;

  res = write_bytes(&ptr, end, (const uint8_t *)msg->reason, msg->reason_len);
  if (res != ECHO_OK)
    return res;

  /* Extra data (32 bytes, optional) */
  if (msg->has_data) {
    res = write_bytes(&ptr, end, msg->data.bytes, 32);
    if (res != ECHO_OK)
      return res;
  }

  *written = (size_t)(ptr - buf);
  return ECHO_OK;
}

echo_result_t msg_reject_deserialize(const uint8_t *buf, size_t buf_len,
                                     msg_reject_t *msg, size_t *consumed) {
  if (!buf || !msg) {
    return ECHO_ERR_NULL_PARAM;
  }

  const uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;
  echo_result_t res;

  /* Message */
  uint64_t msg_len;
  size_t varint_len;
  res = varint_read(ptr, (size_t)(end - ptr), &msg_len, &varint_len);
  if (res != ECHO_OK)
    return res;
  ptr += varint_len;

  if (msg_len > COMMAND_LEN) {
    return ECHO_ERR_INVALID_FORMAT;
  }

  res = read_bytes(&ptr, end, (uint8_t *)msg->message, (size_t)msg_len);
  if (res != ECHO_OK)
    return res;

  /* Null-terminate */
  if (msg_len < COMMAND_LEN) {
    msg->message[msg_len] = '\0';
  }

  /* Reject code */
  res = read_u8(&ptr, end, &msg->ccode);
  if (res != ECHO_OK)
    return res;

  /* Reason */
  uint64_t reason_len;
  res = varint_read(ptr, (size_t)(end - ptr), &reason_len, &varint_len);
  if (res != ECHO_OK)
    return res;
  ptr += varint_len;

  if (reason_len >= 256) {
    return ECHO_ERR_INVALID_FORMAT;
  }

  msg->reason_len = (size_t)reason_len;
  res = read_bytes(&ptr, end, (uint8_t *)msg->reason, msg->reason_len);
  if (res != ECHO_OK)
    return res;

  /* Null-terminate */
  if (msg->reason_len < 256) {
    msg->reason[msg->reason_len] = '\0';
  }

  /* Extra data (optional) */
  if (ptr + 32 <= end) {
    res = read_bytes(&ptr, end, msg->data.bytes, 32);
    if (res != ECHO_OK)
      return res;
    msg->has_data = ECHO_TRUE;
  } else {
    msg->has_data = ECHO_FALSE;
  }

  if (consumed) {
    *consumed = (size_t)(ptr - buf);
  }

  return ECHO_OK;
}

/* ========================================================================
 * feefilter Message Serialization
 * ======================================================================== */

echo_result_t msg_feefilter_serialize(const msg_feefilter_t *msg, uint8_t *buf,
                                      size_t buf_len, size_t *written) {
  if (!msg || !buf || !written) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (buf_len < 8) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }

  uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;

  echo_result_t res = write_u64_le(&ptr, end, msg->feerate);
  if (res != ECHO_OK)
    return res;

  *written = 8;
  return ECHO_OK;
}

echo_result_t msg_feefilter_deserialize(const uint8_t *buf, size_t buf_len,
                                        msg_feefilter_t *msg,
                                        size_t *consumed) {
  if (!buf || !msg) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (buf_len < 8) {
    return ECHO_ERR_TRUNCATED;
  }

  const uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;

  echo_result_t res = read_u64_le(&ptr, end, &msg->feerate);
  if (res != ECHO_OK)
    return res;

  if (consumed) {
    *consumed = 8;
  }

  return ECHO_OK;
}

/* ========================================================================
 * sendcmpct Message Serialization
 * ======================================================================== */

echo_result_t msg_sendcmpct_serialize(const msg_sendcmpct_t *msg, uint8_t *buf,
                                      size_t buf_len, size_t *written) {
  if (!msg || !buf || !written) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (buf_len < 9) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }

  uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;
  echo_result_t res;

  /* Announce flag (1 byte) */
  res = write_u8(&ptr, end, msg->announce ? 1 : 0);
  if (res != ECHO_OK)
    return res;

  /* Version (8 bytes) */
  res = write_u64_le(&ptr, end, msg->version);
  if (res != ECHO_OK)
    return res;

  *written = 9;
  return ECHO_OK;
}

echo_result_t msg_sendcmpct_deserialize(const uint8_t *buf, size_t buf_len,
                                        msg_sendcmpct_t *msg,
                                        size_t *consumed) {
  if (!buf || !msg) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (buf_len < 9) {
    return ECHO_ERR_TRUNCATED;
  }

  const uint8_t *ptr = buf;
  const uint8_t *end = buf + buf_len;
  echo_result_t res;

  /* Announce flag */
  uint8_t announce_byte;
  res = read_u8(&ptr, end, &announce_byte);
  if (res != ECHO_OK)
    return res;
  msg->announce = announce_byte ? ECHO_TRUE : ECHO_FALSE;

  /* Version */
  res = read_u64_le(&ptr, end, &msg->version);
  if (res != ECHO_OK)
    return res;

  if (consumed) {
    *consumed = 9;
  }

  return ECHO_OK;
}
