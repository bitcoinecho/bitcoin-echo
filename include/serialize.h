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
uint32_t deserialize_u32_le(const uint8_t *p);

/*
 * Deserialize a uint64_t from buffer (little-endian).
 * For trusted data only - no bounds checking performed.
 */
uint64_t deserialize_u64_le(const uint8_t *p);

/*
 * Serialize a uint32_t to buffer (little-endian).
 * Buffer must have at least 4 bytes available.
 */
void serialize_u32_le(uint8_t *p, uint32_t v);

/*
 * Serialize a uint64_t to buffer (little-endian).
 * Buffer must have at least 8 bytes available.
 */
void serialize_u64_le(uint8_t *p, uint64_t v);

#endif /* ECHO_SERIALIZE_H */
