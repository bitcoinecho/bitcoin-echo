/*
 * Bitcoin Echo — Serialization Primitives
 *
 * This header defines functions for encoding and decoding Bitcoin's
 * variable-length integer format (CompactSize).
 *
 * CompactSize encoding:
 *   0x00-0xFC:       1 byte  (value as-is)
 *   0xFD + 2 bytes:  3 bytes (for values 253 to 65535)
 *   0xFE + 4 bytes:  5 bytes (for values 65536 to 4294967295)
 *   0xFF + 8 bytes:  9 bytes (for values > 4294967295)
 *
 * All multi-byte values are little-endian.
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_SERIALIZE_H
#define ECHO_SERIALIZE_H

#include "echo_types.h"
#include <stdint.h>

/*
 * CompactSize prefix bytes.
 */
#define VARINT_PREFIX_16 0xFD /* Followed by 2-byte value */
#define VARINT_PREFIX_32 0xFE /* Followed by 4-byte value */
#define VARINT_PREFIX_64 0xFF /* Followed by 8-byte value */

/*
 * Maximum CompactSize value (for transaction counts, etc.)
 * Some contexts limit this further, but the format supports full uint64_t.
 */
#define VARINT_MAX UINT64_MAX

/*
 * Compute the encoded size of a varint.
 *
 * Parameters:
 *   value - The value to encode
 *
 * Returns:
 *   The number of bytes required to encode this value (1, 3, 5, or 9).
 */
size_t varint_size(uint64_t value);

/*
 * Write a varint to a buffer.
 *
 * Parameters:
 *   buf      - Output buffer (must have capacity for varint_size(value) bytes)
 *   buf_len  - Size of output buffer
 *   value    - The value to encode
 *   written  - Output: number of bytes written (may be NULL)
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if buf is NULL
 *   ECHO_ERR_BUFFER_TOO_SMALL if buf_len is insufficient
 */
echo_result_t varint_write(uint8_t *buf, size_t buf_len, uint64_t value,
                           size_t *written);

/*
 * Read a varint from a buffer.
 *
 * This function performs strict validation:
 *   - Values 0-252 must NOT use a prefix (non-canonical encoding rejected)
 *   - Values 253-65535 must use 0xFD prefix (non-canonical encoding rejected)
 *   - Values 65536-4294967295 must use 0xFE prefix
 *   - Larger values must use 0xFF prefix
 *
 * Parameters:
 *   buf       - Input buffer
 *   buf_len   - Size of input buffer
 *   value     - Output: the decoded value
 *   consumed  - Output: number of bytes consumed (may be NULL)
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if buf or value is NULL
 *   ECHO_ERR_TRUNCATED if buffer too short for complete varint
 *   ECHO_ERR_INVALID_FORMAT if non-canonical encoding detected
 */
echo_result_t varint_read(const uint8_t *buf, size_t buf_len, uint64_t *value,
                          size_t *consumed);

/*
 * Deserialize a uint32_t from buffer (little-endian).
 * For trusted data only - no bounds checking performed.
 */
static inline uint32_t deserialize_u32_le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/*
 * Deserialize a uint64_t from buffer (little-endian).
 * For trusted data only - no bounds checking performed.
 */
static inline uint64_t deserialize_u64_le(const uint8_t *p) {
  return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
         ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
         ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
         ((uint64_t)p[7] << 56);
}

/*
 * Serialize a uint32_t to buffer (little-endian).
 * Buffer must have at least 4 bytes available.
 */
static inline void serialize_u32_le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/*
 * Serialize a uint64_t to buffer (little-endian).
 * Buffer must have at least 8 bytes available.
 */
static inline void serialize_u64_le(uint8_t *p, uint64_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
  p[4] = (uint8_t)((v >> 32) & 0xFF);
  p[5] = (uint8_t)((v >> 40) & 0xFF);
  p[6] = (uint8_t)((v >> 48) & 0xFF);
  p[7] = (uint8_t)((v >> 56) & 0xFF);
}

#endif /* ECHO_SERIALIZE_H */
