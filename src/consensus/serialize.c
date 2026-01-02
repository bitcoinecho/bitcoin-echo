/*
 * Bitcoin Echo — Serialization Primitives
 *
 * Implementation of Bitcoin's variable-length integer encoding (CompactSize).
 *
 * Build once. Build right. Stop.
 */

#include "serialize.h"
#include "echo_types.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Compute the encoded size of a varint.
 */
size_t varint_size(uint64_t value) {
  if (value <= 0xFC) {
    return 1;
  } else if (value <= 0xFFFF) {
    return 3;
  } else if (value <= 0xFFFFFFFF) {
    return 5;
  } else {
    return 9;
  }
}

/*
 * Write a varint to a buffer.
 */
echo_result_t varint_write(uint8_t *buf, size_t buf_len, uint64_t value,
                           size_t *written) {
  size_t size;

  if (buf == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  size = varint_size(value);

  if (buf_len < size) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }

  if (value <= 0xFC) {
    /* Single byte encoding */
    buf[0] = (uint8_t)value;
  } else if (value <= 0xFFFF) {
    /* 0xFD prefix + 2 bytes little-endian */
    buf[0] = VARINT_PREFIX_16;
    buf[1] = (uint8_t)(value & 0xFF);
    buf[2] = (uint8_t)((value >> 8) & 0xFF);
  } else if (value <= 0xFFFFFFFF) {
    /* 0xFE prefix + 4 bytes little-endian */
    buf[0] = VARINT_PREFIX_32;
    buf[1] = (uint8_t)(value & 0xFF);
    buf[2] = (uint8_t)((value >> 8) & 0xFF);
    buf[3] = (uint8_t)((value >> 16) & 0xFF);
    buf[4] = (uint8_t)((value >> 24) & 0xFF);
  } else {
    /* 0xFF prefix + 8 bytes little-endian */
    buf[0] = VARINT_PREFIX_64;
    buf[1] = (uint8_t)(value & 0xFF);
    buf[2] = (uint8_t)((value >> 8) & 0xFF);
    buf[3] = (uint8_t)((value >> 16) & 0xFF);
    buf[4] = (uint8_t)((value >> 24) & 0xFF);
    buf[5] = (uint8_t)((value >> 32) & 0xFF);
    buf[6] = (uint8_t)((value >> 40) & 0xFF);
    buf[7] = (uint8_t)((value >> 48) & 0xFF);
    buf[8] = (uint8_t)((value >> 56) & 0xFF);
  }

  if (written != NULL) {
    *written = size;
  }

  return ECHO_OK;
}

/*
 * Read a varint from a buffer (strict canonical checking).
 */
echo_result_t varint_read(const uint8_t *buf, size_t buf_len, uint64_t *value,
                          size_t *consumed) {
  uint64_t result;
  size_t size;

  if (buf == NULL || value == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (buf_len < 1) {
    return ECHO_ERR_TRUNCATED;
  }

  if (buf[0] <= 0xFC) {
    /* Single byte value */
    result = buf[0];
    size = 1;
  } else if (buf[0] == VARINT_PREFIX_16) {
    /* 2-byte value */
    if (buf_len < 3) {
      return ECHO_ERR_TRUNCATED;
    }
    result = (uint64_t)buf[1] | ((uint64_t)buf[2] << 8);
    size = 3;

    /* Canonical check: value must require this encoding */
    if (result < 0xFD) {
      return ECHO_ERR_INVALID_FORMAT;
    }
  } else if (buf[0] == VARINT_PREFIX_32) {
    /* 4-byte value */
    if (buf_len < 5) {
      return ECHO_ERR_TRUNCATED;
    }
    result = (uint64_t)buf[1] | ((uint64_t)buf[2] << 8) |
             ((uint64_t)buf[3] << 16) | ((uint64_t)buf[4] << 24);
    size = 5;

    /* Canonical check: value must require this encoding */
    if (result <= 0xFFFF) {
      return ECHO_ERR_INVALID_FORMAT;
    }
  } else {
    /* 8-byte value (buf[0] == VARINT_PREFIX_64) */
    if (buf_len < 9) {
      return ECHO_ERR_TRUNCATED;
    }
    result = (uint64_t)buf[1] | ((uint64_t)buf[2] << 8) |
             ((uint64_t)buf[3] << 16) | ((uint64_t)buf[4] << 24) |
             ((uint64_t)buf[5] << 32) | ((uint64_t)buf[6] << 40) |
             ((uint64_t)buf[7] << 48) | ((uint64_t)buf[8] << 56);
    size = 9;

    /* Canonical check: value must require this encoding */
    if (result <= 0xFFFFFFFF) {
      return ECHO_ERR_INVALID_FORMAT;
    }
  }

  *value = result;
  if (consumed != NULL) {
    *consumed = size;
  }

  return ECHO_OK;
}

/*
 * Deserialize a 32-bit little-endian value.
 * For trusted data only - no bounds checking performed.
 */
uint32_t deserialize_u32_le(const uint8_t *p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/*
 * Deserialize a 64-bit little-endian value.
 * For trusted data only - no bounds checking performed.
 */
uint64_t deserialize_u64_le(const uint8_t *p) {
  return (uint64_t)p[0] |
         ((uint64_t)p[1] << 8) |
         ((uint64_t)p[2] << 16) |
         ((uint64_t)p[3] << 24) |
         ((uint64_t)p[4] << 32) |
         ((uint64_t)p[5] << 40) |
         ((uint64_t)p[6] << 48) |
         ((uint64_t)p[7] << 56);
}

/*
 * Serialize a 32-bit little-endian value.
 * Buffer must have at least 4 bytes available.
 */
void serialize_u32_le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/*
 * Serialize a 64-bit little-endian value.
 * Buffer must have at least 8 bytes available.
 */
void serialize_u64_le(uint8_t *p, uint64_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
  p[4] = (uint8_t)((v >> 32) & 0xFF);
  p[5] = (uint8_t)((v >> 40) & 0xFF);
  p[6] = (uint8_t)((v >> 48) & 0xFF);
  p[7] = (uint8_t)((v >> 56) & 0xFF);
}
