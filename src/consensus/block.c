/*
 * Bitcoin Echo — Block Data Structures
 *
 * Implementation of block parsing, serialization, and hash computation.
 *
 * Build once. Build right. Stop.
 */

#include "block.h"
#include "echo_types.h"
#include "serialize.h"
#include "sha256.h"
#include "tx.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


/*
 * Initialize a block structure.
 */
void block_init(block_t *block) {
  if (block == NULL)
    return;

  memset(&block->header, 0, sizeof(block_header_t));
  block->txs = NULL;
  block->tx_count = 0;
}

/*
 * Free all memory owned by a block.
 */
void block_free(block_t *block) {
  size_t i;

  if (block == NULL)
    return;

  if (block->txs != NULL) {
    for (i = 0; i < block->tx_count; i++) {
      tx_free(&block->txs[i]);
    }
    free(block->txs);
    block->txs = NULL;
  }
  block->tx_count = 0;
}

/*
 * Parse a block header from raw bytes.
 */
echo_result_t block_header_parse(const uint8_t *data, size_t data_len,
                                 block_header_t *header) {
  if (data == NULL || header == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (data_len < BLOCK_HEADER_SIZE) {
    return ECHO_ERR_TRUNCATED;
  }

  /* Version (4 bytes) */
  header->version = (int32_t)deserialize_u32_le(data);

  /* Previous block hash (32 bytes) */
  memcpy(header->prev_hash.bytes, data + 4, 32);

  /* Merkle root (32 bytes) */
  memcpy(header->merkle_root.bytes, data + 36, 32);

  /* Timestamp (4 bytes) */
  header->timestamp = deserialize_u32_le(data + 68);

  /* Bits (4 bytes) */
  header->bits = deserialize_u32_le(data + 72);

  /* Nonce (4 bytes) */
  header->nonce = deserialize_u32_le(data + 76);

  return ECHO_OK;
}

/*
 * Serialize a block header to bytes.
 */
echo_result_t block_header_serialize(const block_header_t *header, uint8_t *buf,
                                     size_t buf_len) {
  if (header == NULL || buf == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (buf_len < BLOCK_HEADER_SIZE) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }

  /* Version (4 bytes) */
  serialize_u32_le(buf, (uint32_t)header->version);

  /* Previous block hash (32 bytes) */
  memcpy(buf + 4, header->prev_hash.bytes, 32);

  /* Merkle root (32 bytes) */
  memcpy(buf + 36, header->merkle_root.bytes, 32);

  /* Timestamp (4 bytes) */
  serialize_u32_le(buf + 68, header->timestamp);

  /* Bits (4 bytes) */
  serialize_u32_le(buf + 72, header->bits);

  /* Nonce (4 bytes) */
  serialize_u32_le(buf + 76, header->nonce);

  return ECHO_OK;
}

/*
 * Compute the block hash.
 */
echo_result_t block_header_hash(const block_header_t *header, hash256_t *hash) {
  uint8_t buf[BLOCK_HEADER_SIZE];
  echo_result_t result;

  if (header == NULL || hash == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  result = block_header_serialize(header, buf, sizeof(buf));
  if (result != ECHO_OK) {
    return result;
  }

  sha256d(buf, BLOCK_HEADER_SIZE, hash->bytes);

  return ECHO_OK;
}

/*
 * Parse a full block from raw bytes.
 */
echo_result_t block_parse(const uint8_t *data, size_t data_len, block_t *block,
                          size_t *consumed) {
  size_t offset = 0;
  uint64_t tx_count;
  size_t var_consumed;
  size_t i;
  echo_result_t result;

  if (data == NULL || block == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  block_init(block);

  /* Parse header */
  result = block_header_parse(data, data_len, &block->header);
  if (result != ECHO_OK) {
    return result;
  }
  offset = BLOCK_HEADER_SIZE;

  /* Parse transaction count */
  result =
      varint_read(data + offset, data_len - offset, &tx_count, &var_consumed);
  if (result != ECHO_OK) {
    return result;
  }
  offset += var_consumed;

  if (tx_count == 0) {
    /* Blocks must have at least one transaction (coinbase) */
    return ECHO_ERR_INVALID_FORMAT;
  }

  /* Allocate transaction array */
  block->tx_count = (size_t)tx_count;
  block->txs = calloc(block->tx_count, sizeof(tx_t));
  if (block->txs == NULL) {
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  /* Parse each transaction */
  for (i = 0; i < block->tx_count; i++) {
    size_t tx_consumed;

    result = tx_parse(data + offset, data_len - offset, &block->txs[i],
                      &tx_consumed);
    if (result != ECHO_OK) {
      block_free(block);
      return result;
    }
    offset += tx_consumed;
  }

  if (consumed != NULL) {
    *consumed = offset;
  }

  return ECHO_OK;
}

/*
 * Compute the serialized size of a full block.
 */
size_t block_serialize_size(const block_t *block) {
  size_t size = 0;
  size_t i;

  if (block == NULL)
    return 0;

  /* Header */
  size += BLOCK_HEADER_SIZE;

  /* Transaction count */
  size += varint_size(block->tx_count);

  /* Transactions */
  for (i = 0; i < block->tx_count; i++) {
    size += tx_serialize_size(&block->txs[i], ECHO_TRUE);
  }

  return size;
}

/*
 * Serialize a full block to bytes.
 */
echo_result_t block_serialize(const block_t *block, uint8_t *buf,
                              size_t buf_len, size_t *written) {
  size_t offset = 0;
  size_t needed;
  size_t var_written;
  size_t i;
  echo_result_t result;

  if (block == NULL || buf == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  needed = block_serialize_size(block);
  if (buf_len < needed) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }

  /* Serialize header */
  result = block_header_serialize(&block->header, buf, buf_len);
  if (result != ECHO_OK) {
    return result;
  }
  offset = BLOCK_HEADER_SIZE;

  /* Serialize transaction count */
  result = varint_write(buf + offset, buf_len - offset, block->tx_count,
                        &var_written);
  if (result != ECHO_OK) {
    return result;
  }
  offset += var_written;

  /* Serialize transactions */
  for (i = 0; i < block->tx_count; i++) {
    size_t tx_written;

    result = tx_serialize(&block->txs[i], ECHO_TRUE, buf + offset,
                          buf_len - offset, &tx_written);
    if (result != ECHO_OK) {
      return result;
    }
    offset += tx_written;
  }

  if (written != NULL) {
    *written = offset;
  }

  return ECHO_OK;
}

/*
 * Compute block weight.
 */
size_t block_weight(const block_t *block) {
  size_t base_size = 0;
  size_t total_size = 0;
  size_t i;

  if (block == NULL)
    return 0;

  /* Header (always counted at full weight) */
  base_size += BLOCK_HEADER_SIZE;
  total_size += BLOCK_HEADER_SIZE;

  /* Transaction count varint */
  base_size += varint_size(block->tx_count);
  total_size += varint_size(block->tx_count);

  /* Transactions */
  for (i = 0; i < block->tx_count; i++) {
    base_size += tx_serialize_size(&block->txs[i], ECHO_FALSE);
    total_size += tx_serialize_size(&block->txs[i], ECHO_TRUE);
  }

  /* Weight = base_size * 3 + total_size */
  return (base_size * 3) + total_size;
}

/*
 * Convert compact "bits" field to 256-bit target.
 */
echo_result_t block_bits_to_target(uint32_t bits, hash256_t *target) {
  uint32_t mantissa;
  uint32_t exponent;
  int i;

  if (target == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Clear target */
  memset(target->bytes, 0, 32);

  /* Extract exponent (high byte) and mantissa (low 3 bytes) */
  exponent = bits >> 24;
  mantissa = bits & 0x00FFFFFF;

  /* Handle negative flag in mantissa (bit 23) */
  if (mantissa & 0x800000) {
    /* Negative targets are invalid, treat as zero */
    return ECHO_OK;
  }

  /* Handle edge cases */
  if (exponent == 0) {
    /* Target is zero */
    return ECHO_OK;
  }

  if (exponent <= 3) {
    /* Mantissa is right-shifted */
    mantissa >>= 8 * (3 - exponent);
    target->bytes[0] = (uint8_t)(mantissa & 0xFF);
    target->bytes[1] = (uint8_t)((mantissa >> 8) & 0xFF);
    target->bytes[2] = (uint8_t)((mantissa >> 16) & 0xFF);
  } else {
    /* Mantissa is placed at byte position (exponent - 3) */
    uint32_t pos = exponent - 3;

    if (pos > 29) {
      /* Would overflow 32 bytes, cap it */
      pos = 29;
    }

    target->bytes[pos] = (uint8_t)(mantissa & 0xFF);
    if (pos + 1 < 32) {
      target->bytes[pos + 1] = (uint8_t)((mantissa >> 8) & 0xFF);
    }
    if (pos + 2 < 32) {
      target->bytes[pos + 2] = (uint8_t)((mantissa >> 16) & 0xFF);
    }
  }

  (void)i; /* Suppress unused variable warning */

  return ECHO_OK;
}

/*
 * Convert 256-bit target to compact "bits" field.
 */
echo_result_t block_target_to_bits(const hash256_t *target, uint32_t *bits) {
  int i;
  int first_nonzero;
  uint32_t mantissa;
  uint32_t exponent;

  if (target == NULL || bits == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Find the first non-zero byte (from most significant) */
  first_nonzero = -1;
  for (i = 31; i >= 0; i--) {
    if (target->bytes[i] != 0) {
      first_nonzero = i;
      break;
    }
  }

  if (first_nonzero < 0) {
    /* Target is zero */
    *bits = 0;
    return ECHO_OK;
  }

  /* Exponent is position + 1 (since we use 3 bytes of mantissa) */
  exponent = (uint32_t)(first_nonzero + 1);

  /* Extract 3 bytes of mantissa */
  if (first_nonzero >= 2) {
    mantissa = ((uint32_t)target->bytes[first_nonzero] << 16) |
               ((uint32_t)target->bytes[first_nonzero - 1] << 8) |
               ((uint32_t)target->bytes[first_nonzero - 2]);
  } else if (first_nonzero >= 1) {
    mantissa = ((uint32_t)target->bytes[first_nonzero] << 16) |
               ((uint32_t)target->bytes[first_nonzero - 1] << 8);
  } else {
    mantissa = (uint32_t)target->bytes[first_nonzero] << 16;
  }

  /* If the high bit of mantissa is set, we need to adjust to avoid
   * it being interpreted as a negative number */
  if (mantissa & 0x800000) {
    mantissa >>= 8;
    exponent++;
  }

  *bits = (exponent << 24) | mantissa;

  return ECHO_OK;
}

/*
 * Check if a block hash meets the target difficulty.
 */
echo_bool_t block_hash_meets_target(const hash256_t *hash,
                                    const hash256_t *target) {
  int i;

  if (hash == NULL || target == NULL) {
    return ECHO_FALSE;
  }

  /* Compare from most significant byte (hash is little-endian) */
  for (i = 31; i >= 0; i--) {
    if (hash->bytes[i] < target->bytes[i]) {
      return ECHO_TRUE; /* hash < target */
    }
    if (hash->bytes[i] > target->bytes[i]) {
      return ECHO_FALSE; /* hash > target */
    }
  }

  /* hash == target */
  return ECHO_TRUE;
}

/*
 * Get the genesis block header (mainnet).
 */
void block_genesis_header(block_header_t *header) {
  /* Genesis block previous hash (all zeros) */
  static const uint8_t genesis_prev_hash[32] = {0};

  /* Genesis block merkle root (hash of single coinbase tx) */
  static const uint8_t genesis_merkle_root[32] = {
      0x3b, 0xa3, 0xed, 0xfd, 0x7a, 0x7b, 0x12, 0xb2, 0x7a, 0xc7, 0x2c,
      0x3e, 0x67, 0x76, 0x8f, 0x61, 0x7f, 0xc8, 0x1b, 0xc3, 0x88, 0x8a,
      0x51, 0x32, 0x3a, 0x9f, 0xb8, 0xaa, 0x4b, 0x1e, 0x5e, 0x4a};

  if (header == NULL)
    return;

  header->version = GENESIS_BLOCK_VERSION;
  memcpy(header->prev_hash.bytes, genesis_prev_hash, 32);
  memcpy(header->merkle_root.bytes, genesis_merkle_root, 32);
  header->timestamp = GENESIS_BLOCK_TIMESTAMP;
  header->bits = GENESIS_BLOCK_BITS;
  header->nonce = GENESIS_BLOCK_NONCE;
}
