/**
 * @file chainstate.c
 * @brief Chain state implementation
 *
 * Implements chain state tracking, including:
 * - 256-bit work arithmetic for chain selection
 * - Block index for tracking headers
 * - Block deltas for atomic apply/revert
 * - UTXO set management during chain transitions
 *
 * Build once. Build right. Stop.
 */

#include "chainstate.h"
#include "block.h"
#include "echo_assert.h"
#include "echo_types.h"
#include "tx.h"
#include "utxo.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Maximum number of block hashes to track by height (for main chain) */
#define MAX_HEIGHT_INDEX 1000000

/* Default size for block index map hash table */
/* Bitcoin mainnet has 800k+ blocks. Need room for headers + fork blocks. */
#define BLOCK_INDEX_MAP_DEFAULT_SIZE (1024 * 1024)

/**
 * Block index map implementation (hash table)
 */
struct block_index_map {
  block_index_t **buckets; /* Hash table buckets */
  size_t bucket_count;     /* Number of buckets */
  size_t count;            /* Number of entries */
};

/**
 * Chain state implementation
 */
struct chainstate {
  chain_tip_t tip;      /* Current chain tip */
  utxo_set_t *utxo_set; /* The UTXO set */

  /* Height-indexed block hashes (main chain only) */
  hash256_t *height_index;      /* Array of block hashes by height */
  size_t height_index_capacity; /* Allocated capacity */

  /* Block index map for fork tracking */
  block_index_map_t *block_map; /* All known block indices */
  block_index_t *tip_index;     /* Block index for current tip */

  /* Delta storage for reorganization undo data */
  block_delta_t **deltas; /* Array of deltas by height */
  size_t deltas_capacity; /* Allocated capacity for deltas */
};

/* ========================================================================
 * Work (256-bit integer) Operations
 * ======================================================================== */

void work256_zero(work256_t *work) {
  ECHO_ASSERT(work != NULL);
  memset(work->bytes, 0, 32);
}

bool work256_is_zero(const work256_t *work) {
  ECHO_ASSERT(work != NULL);

  for (int i = 0; i < 32; i++) {
    if (work->bytes[i] != 0) {
      return false;
    }
  }
  return true;
}

int work256_compare(const work256_t *a, const work256_t *b) {
  ECHO_ASSERT(a != NULL);
  ECHO_ASSERT(b != NULL);

  /* Compare from most significant byte (big-endian comparison) */
  /* Note: work is stored little-endian, so compare from index 31 down */
  for (int i = 31; i >= 0; i--) {
    if (a->bytes[i] < b->bytes[i]) {
      return -1;
    }
    if (a->bytes[i] > b->bytes[i]) {
      return 1;
    }
  }
  return 0;
}

void work256_add(const work256_t *a, const work256_t *b, work256_t *result) {
  ECHO_ASSERT(a != NULL);
  ECHO_ASSERT(b != NULL);
  ECHO_ASSERT(result != NULL);

  uint16_t carry = 0;
  for (int i = 0; i < 32; i++) {
    uint16_t sum = (uint16_t)a->bytes[i] + (uint16_t)b->bytes[i] + carry;
    result->bytes[i] = (uint8_t)(sum & 0xFF);
    carry = sum >> 8;
  }
  /* Overflow is ignored (would indicate > 2^256 total work, impossible) */
}

echo_result_t work256_sub(const work256_t *a, const work256_t *b,
                          work256_t *result) {
  ECHO_ASSERT(a != NULL);
  ECHO_ASSERT(b != NULL);
  ECHO_ASSERT(result != NULL);

  /* Check for underflow */
  if (work256_compare(a, b) < 0) {
    return ECHO_ERR_UNDERFLOW;
  }

  int16_t borrow = 0;
  for (int i = 0; i < 32; i++) {
    int16_t diff = (int16_t)(a->bytes[i] - b->bytes[i] - borrow);
    if (diff < 0) {
      diff += 256;
      borrow = 1;
    } else {
      borrow = 0;
    }
    result->bytes[i] = (uint8_t)diff;
  }

  return ECHO_OK;
}

echo_result_t work256_from_bits(uint32_t bits, work256_t *work) {
  ECHO_ASSERT(work != NULL);

  /* Convert bits to target */
  hash256_t target;
  echo_result_t result = block_bits_to_target(bits, &target);
  if (result != ECHO_OK) {
    return result;
  }

  /* Check for zero target (would cause division by zero) */
  bool is_zero = true;
  for (int i = 0; i < 32; i++) {
    if (target.bytes[i] != 0) {
      is_zero = false;
      break;
    }
  }
  if (is_zero) {
    work256_zero(work);
    return ECHO_OK;
  }

  /* Work = 2^256 / (target + 1)
   *
   * This is a simplified calculation. For maximum precision, we'd need
   * full 256-bit division. Instead, we use a reasonable approximation:
   *
   * We compute this as: (~target / (target + 1)) + 1
   * which equals 2^256 / (target + 1) when target < 2^256 - 1
   *
   * For simplicity, we approximate using the leading non-zero bytes.
   */

  /* Find the position of the most significant non-zero byte in target */
  int msb_pos = 31;
  while (msb_pos >= 0 && target.bytes[msb_pos] == 0) {
    msb_pos--;
  }

  if (msb_pos < 0) {
    /* Target is zero, work is maximum (shouldn't happen) */
    memset(work->bytes, 0xFF, 32);
    return ECHO_OK;
  }

  /* For a target with N leading zeros, work ~= 2^(256-N*8) / mantissa
   *
   * We'll use a more accurate method: use 64-bit arithmetic on the
   * top bytes and scale appropriately.
   */

  /* Extract top 8 bytes of target as 64-bit value */
  uint64_t target_top = 0;
  for (int i = 0; i < 8 && (msb_pos - i) >= 0; i++) {
    target_top = (target_top << 8) | target.bytes[msb_pos - i];
  }

  /* Avoid division by zero */
  if (target_top == 0) {
    target_top = 1;
  }

  /* The work is approximately 2^256 / target
   * If target occupies bytes [0, msb_pos], then target ~= target_top *
   * 2^((msb_pos-7)*8) So work ~= 2^256 / (target_top * 2^((msb_pos-7)*8)) =
   * (2^256 / 2^((msb_pos-7)*8)) / target_top = 2^(256 - (msb_pos-7)*8) /
   * target_top = 2^(256 - 8*msb_pos + 56) / target_top = 2^(312 - 8*msb_pos) /
   * target_top
   *
   * Let shift = 312 - 8*msb_pos = 8*(39 - msb_pos)
   * Then work ~= 2^shift / target_top
   */

  int shift_bits = 312 - 8 * msb_pos;

  /* Compute work_top = 2^64 / target_top (approximation of top 64 bits) */
  uint64_t work_top = UINT64_MAX / target_top;

  /* Position this in the 256-bit result
   * The shift tells us where the MSB of work_top should go.
   * If shift_bits = 64, work_top goes in bytes [0..7]
   * If shift_bits = 128, work_top goes in bytes [8..15]
   * etc.
   */

  work256_zero(work);

  /* byte position = (shift_bits - 64) / 8 = shift_bits/8 - 8 */
  int byte_pos = shift_bits / 8 - 8;
  if (byte_pos < 0)
    byte_pos = 0;
  if (byte_pos > 24)
    byte_pos = 24;

  /* Store work_top at the appropriate position */
  for (int i = 0; i < 8 && (byte_pos + i) < 32; i++) {
    work->bytes[byte_pos + i] = (uint8_t)(work_top >> (i * 8));
  }

  return ECHO_OK;
}

/* ========================================================================
 * Block Index Operations
 * ======================================================================== */

block_index_t *block_index_create(const block_header_t *header,
                                  block_index_t *prev) {
  ECHO_ASSERT(header != NULL);

  block_index_t *index = malloc(sizeof(block_index_t));
  if (index == NULL) {
    return NULL;
  }

  /* Compute block hash */
  if (block_header_hash(header, &index->hash) != ECHO_OK) {
    free(index);
    return NULL;
  }

  index->prev_hash = header->prev_hash;
  index->timestamp = header->timestamp;
  index->bits = header->bits;
  index->prev = prev;
  index->on_main_chain = false;

  /* Set height */
  if (prev == NULL) {
    index->height = 0; /* Genesis */
  } else {
    index->height = prev->height + 1;
  }

  /* Calculate cumulative chainwork */
  work256_t block_work;
  if (work256_from_bits(header->bits, &block_work) != ECHO_OK) {
    free(index);
    return NULL;
  }

  if (prev == NULL) {
    index->chainwork = block_work;
  } else {
    work256_add(&prev->chainwork, &block_work, &index->chainwork);
  }

  /* Block data not stored yet (header only) */
  index->data_file = BLOCK_DATA_NOT_STORED;
  index->data_pos = 0;

  return index;
}

void block_index_destroy(block_index_t *index) {
  /* Note: does NOT free prev (not owned) */
  free(index);
}

/* ========================================================================
 * Block Delta Operations
 * ======================================================================== */

block_delta_t *block_delta_create(const hash256_t *block_hash,
                                  uint32_t height) {
  ECHO_ASSERT(block_hash != NULL);

  block_delta_t *delta = malloc(sizeof(block_delta_t));
  if (delta == NULL) {
    return NULL;
  }

  delta->block_hash = *block_hash;
  delta->height = height;
  delta->created = NULL;
  delta->created_count = 0;
  delta->spent = NULL;
  delta->spent_count = 0;

  return delta;
}

void block_delta_destroy(block_delta_t *delta) {
  if (delta == NULL) {
    return;
  }

  /* Free created outpoints array */
  free(delta->created);

  /* Free spent UTXO entries */
  for (size_t i = 0; i < delta->spent_count; i++) {
    utxo_entry_destroy(delta->spent[i]);
  }
  free(delta->spent);

  free(delta);
}

echo_result_t block_delta_add_created(block_delta_t *delta,
                                      const outpoint_t *outpoint) {
  ECHO_ASSERT(delta != NULL);
  ECHO_ASSERT(outpoint != NULL);

  /* Grow array */
  outpoint_t *new_created =
      realloc(delta->created, (delta->created_count + 1) * sizeof(outpoint_t));
  if (new_created == NULL) {
    return ECHO_ERR_NOMEM;
  }

  delta->created = new_created;
  delta->created[delta->created_count] = *outpoint;
  delta->created_count++;

  return ECHO_OK;
}

echo_result_t block_delta_add_spent(block_delta_t *delta,
                                    const utxo_entry_t *entry) {
  ECHO_ASSERT(delta != NULL);
  ECHO_ASSERT(entry != NULL);

  /* Clone the entry for undo */
  utxo_entry_t *cloned = utxo_entry_clone(entry);
  if (cloned == NULL) {
    return ECHO_ERR_NOMEM;
  }

  /* Grow array */
  utxo_entry_t **new_spent =
      realloc(delta->spent, (delta->spent_count + 1) * sizeof(utxo_entry_t *));
  if (new_spent == NULL) {
    utxo_entry_destroy(cloned);
    return ECHO_ERR_NOMEM;
  }

  delta->spent = new_spent;
  delta->spent[delta->spent_count] = cloned;
  delta->spent_count++;

  return ECHO_OK;
}

/* ========================================================================
 * Chain State Operations
 * ======================================================================== */

chainstate_t *chainstate_create(void) {
  chainstate_t *state = malloc(sizeof(chainstate_t));
  if (state == NULL) {
    return NULL;
  }

  /* Initialize tip to genesis (height 0, no hash yet) */
  memset(&state->tip.hash, 0, 32);
  state->tip.height = 0;
  work256_zero(&state->tip.chainwork);

  /* Create UTXO set */
  state->utxo_set = utxo_set_create(0);
  if (state->utxo_set == NULL) {
    free(state);
    return NULL;
  }

  /* Initialize height index */
  state->height_index_capacity = 1024; /* Initial capacity */
  state->height_index = calloc(state->height_index_capacity, sizeof(hash256_t));
  if (state->height_index == NULL) {
    utxo_set_destroy(state->utxo_set);
    free(state);
    return NULL;
  }

  /* Create block index map */
  state->block_map = block_index_map_create(0);
  if (state->block_map == NULL) {
    free(state->height_index);
    utxo_set_destroy(state->utxo_set);
    free(state);
    return NULL;
  }

  state->tip_index = NULL;

  /* Initialize delta storage */
  state->deltas_capacity = 1024;
  state->deltas = calloc(state->deltas_capacity, sizeof(block_delta_t *));
  if (state->deltas == NULL) {
    block_index_map_destroy(state->block_map);
    free(state->height_index);
    utxo_set_destroy(state->utxo_set);
    free(state);
    return NULL;
  }

  return state;
}

void chainstate_destroy(chainstate_t *state) {
  if (state == NULL) {
    return;
  }

  /* Free deltas */
  if (state->deltas != NULL) {
    for (size_t i = 0; i < state->deltas_capacity; i++) {
      if (state->deltas[i] != NULL) {
        block_delta_destroy(state->deltas[i]);
      }
    }
    free(state->deltas);
  }

  block_index_map_destroy(state->block_map);
  utxo_set_destroy(state->utxo_set);
  free(state->height_index);
  free(state);
}

echo_result_t chainstate_get_tip(const chainstate_t *state, chain_tip_t *tip) {
  ECHO_ASSERT(state != NULL);
  ECHO_ASSERT(tip != NULL);

  *tip = state->tip;
  return ECHO_OK;
}

uint32_t chainstate_get_height(const chainstate_t *state) {
  ECHO_ASSERT(state != NULL);
  return state->tip.height;
}

const utxo_set_t *chainstate_get_utxo_set(const chainstate_t *state) {
  ECHO_ASSERT(state != NULL);
  return state->utxo_set;
}

utxo_set_t *chainstate_get_utxo_set_mutable(chainstate_t *state) {
  ECHO_ASSERT(state != NULL);
  return state->utxo_set;
}

/**
 * Internal helper: grow height index if needed
 */
static echo_result_t chainstate_ensure_height_capacity(chainstate_t *state,
                                                       uint32_t height) {
  if (height < state->height_index_capacity) {
    return ECHO_OK;
  }

  /* Double capacity until it fits */
  size_t new_capacity = state->height_index_capacity;
  while (new_capacity <= height) {
    new_capacity *= 2;
    if (new_capacity > MAX_HEIGHT_INDEX) {
      new_capacity = MAX_HEIGHT_INDEX;
      break;
    }
  }

  if (height >= new_capacity) {
    return ECHO_ERR_OVERFLOW;
  }

  hash256_t *new_index =
      realloc(state->height_index, new_capacity * sizeof(hash256_t));
  if (new_index == NULL) {
    return ECHO_ERR_NOMEM;
  }

  /* Zero out new entries */
  memset(new_index + state->height_index_capacity, 0,
         (new_capacity - state->height_index_capacity) * sizeof(hash256_t));

  state->height_index = new_index;
  state->height_index_capacity = new_capacity;

  return ECHO_OK;
}

echo_result_t chainstate_apply_block(chainstate_t *state,
                                     const block_header_t *header,
                                     const tx_t *txs, size_t tx_count,
                                     block_delta_t **delta_out) {
  ECHO_ASSERT(state != NULL);
  ECHO_ASSERT(header != NULL);
  ECHO_ASSERT(txs != NULL || tx_count == 0);

  /* Compute block hash */
  hash256_t block_hash;
  echo_result_t result = block_header_hash(header, &block_hash);
  if (result != ECHO_OK) {
    return result;
  }

  /* Determine new height */
  uint32_t new_height;
  if (state->tip.height == 0 && work256_is_zero(&state->tip.chainwork)) {
    /* First block (genesis or first block after genesis) */
    /* Check if this is genesis (prev_hash all zeros) */
    bool is_genesis = true;
    for (int i = 0; i < 32; i++) {
      if (header->prev_hash.bytes[i] != 0) {
        is_genesis = false;
        break;
      }
    }
    new_height = is_genesis ? 0 : 1;
  } else {
    /* Verify this block connects to current tip */
    if (memcmp(header->prev_hash.bytes, state->tip.hash.bytes, 32) != 0) {
      return ECHO_ERR_INVALID_BLOCK; /* Doesn't connect */
    }
    new_height = state->tip.height + 1;
  }

  /* Ensure height index capacity */
  result = chainstate_ensure_height_capacity(state, new_height);
  if (result != ECHO_OK) {
    return result;
  }

  /* Create delta if requested */
  block_delta_t *delta = NULL;
  if (delta_out != NULL) {
    delta = block_delta_create(&block_hash, new_height);
    if (delta == NULL) {
      return ECHO_ERR_NOMEM;
    }
  }

  /* Process all transactions */
  for (size_t tx_idx = 0; tx_idx < tx_count; tx_idx++) {
    const tx_t *tx = &txs[tx_idx];
    bool is_coinbase = (tx_idx == 0);

    /* Spend inputs (skip for coinbase) */
    if (!is_coinbase) {
      for (size_t in_idx = 0; in_idx < tx->input_count; in_idx++) {
        const outpoint_t *outpoint = &tx->inputs[in_idx].prevout;

        /* Record spent UTXO for undo */
        if (delta != NULL) {
          const utxo_entry_t *spent_entry =
              utxo_set_lookup(state->utxo_set, outpoint);
          if (spent_entry != NULL) {
            result = block_delta_add_spent(delta, spent_entry);
            if (result != ECHO_OK) {
              block_delta_destroy(delta);
              return result;
            }
          }
        }

        /* Remove from UTXO set */
        utxo_set_remove(state->utxo_set, outpoint);
      }
    }

    /* Create outputs */
    hash256_t txid;
    tx_compute_txid(tx, &txid);

    for (size_t out_idx = 0; out_idx < tx->output_count; out_idx++) {
      const tx_output_t *output = &tx->outputs[out_idx];

      outpoint_t outpoint;
      outpoint.txid = txid;
      outpoint.vout = (uint32_t)out_idx;

      /* Record created outpoint for undo */
      if (delta != NULL) {
        result = block_delta_add_created(delta, &outpoint);
        if (result != ECHO_OK) {
          block_delta_destroy(delta);
          return result;
        }
      }

      /* Add to UTXO set */
      utxo_entry_t *entry =
          utxo_entry_create(&outpoint, output->value, output->script_pubkey,
                            output->script_pubkey_len, new_height, is_coinbase);

      if (entry == NULL) {
        block_delta_destroy(delta);
        return ECHO_ERR_NOMEM;
      }

      result = utxo_set_insert(state->utxo_set, entry);
      utxo_entry_destroy(entry);

      if (result != ECHO_OK && result != ECHO_ERR_EXISTS) {
        block_delta_destroy(delta);
        return result;
      }
    }
  }

  /* Update chain tip */
  state->tip.hash = block_hash;
  state->tip.height = new_height;

  /* Calculate and add block work */
  work256_t block_work;
  result = work256_from_bits(header->bits, &block_work);
  if (result != ECHO_OK) {
    block_delta_destroy(delta);
    return result;
  }
  work256_add(&state->tip.chainwork, &block_work, &state->tip.chainwork);

  /* Update height index */
  state->height_index[new_height] = block_hash;

  /* Return delta if requested */
  if (delta_out != NULL) {
    *delta_out = delta;
  }

  return ECHO_OK;
}

echo_result_t chainstate_revert_block(chainstate_t *state,
                                      const block_delta_t *delta) {
  ECHO_ASSERT(state != NULL);
  ECHO_ASSERT(delta != NULL);

  /* Verify we're reverting the tip */
  if (memcmp(state->tip.hash.bytes, delta->block_hash.bytes, 32) != 0) {
    return ECHO_ERR_INVALID_BLOCK;
  }

  if (state->tip.height != delta->height) {
    return ECHO_ERR_INVALID_BLOCK;
  }

  /* Remove created UTXOs */
  for (size_t i = 0; i < delta->created_count; i++) {
    utxo_set_remove(state->utxo_set, &delta->created[i]);
  }

  /* Restore spent UTXOs */
  for (size_t i = 0; i < delta->spent_count; i++) {
    utxo_set_insert(state->utxo_set, delta->spent[i]);
  }

  /* Update chain tip */
  if (delta->height == 0) {
    /* Reverting genesis - reset to empty state */
    memset(&state->tip.hash, 0, 32);
    state->tip.height = 0;
    work256_zero(&state->tip.chainwork);
  } else {
    /* Set tip to previous block */
    state->tip.hash = state->height_index[delta->height - 1];
    state->tip.height = delta->height - 1;

    /* Recalculate chainwork - need to subtract this block's work
     * For now, we don't track per-block work in the delta,
     * so we'd need to recompute from height_index.
     * This is a simplification - a full implementation would store
     * the previous chainwork in the delta.
     */
    /* TODO: For now, leave chainwork as-is. A proper implementation
     * would either store previous chainwork in delta or recompute. */
  }

  return ECHO_OK;
}

bool chainstate_is_on_main_chain(const chainstate_t *state,
                                 const hash256_t *hash) {
  ECHO_ASSERT(state != NULL);
  ECHO_ASSERT(hash != NULL);

  /* Search height index */
  for (uint32_t h = 0;
       h <= state->tip.height && h < state->height_index_capacity; h++) {
    if (memcmp(state->height_index[h].bytes, hash->bytes, 32) == 0) {
      return true;
    }
  }

  return false;
}

echo_result_t chainstate_get_block_at_height(const chainstate_t *state,
                                             uint32_t height, hash256_t *hash) {
  ECHO_ASSERT(state != NULL);
  ECHO_ASSERT(hash != NULL);

  if (height > state->tip.height) {
    return ECHO_ERR_NOT_FOUND;
  }

  if (height >= state->height_index_capacity) {
    return ECHO_ERR_NOT_FOUND;
  }

  *hash = state->height_index[height];
  return ECHO_OK;
}

const utxo_entry_t *chainstate_lookup_utxo(const chainstate_t *state,
                                           const outpoint_t *outpoint) {
  ECHO_ASSERT(state != NULL);
  ECHO_ASSERT(outpoint != NULL);

  return utxo_set_lookup(state->utxo_set, outpoint);
}

void chainstate_get_stats(const chainstate_t *state, size_t *utxo_count,
                          int64_t *total_amount) {
  ECHO_ASSERT(state != NULL);

  if (utxo_count != NULL) {
    *utxo_count = utxo_set_size(state->utxo_set);
  }

  if (total_amount != NULL) {
    /* Iterate through UTXO set to sum amounts */
    /* For now, we'll use the foreach function with a callback */
    /* This is a simplification - a full implementation might cache this */
    *total_amount = 0;

    /* Note: We can't easily iterate here without exposing internals
     * or adding a sum function to UTXO set. For now, return 0.
     * A proper implementation would add utxo_set_total_value() */
  }
}

/* ========================================================================
 * Block Index Map Implementation
 * ======================================================================== */

/**
 * Compute hash table bucket index from block hash.
 * Uses first 8 bytes of hash as a simple hash function.
 */
static size_t block_index_map_hash(const hash256_t *hash, size_t bucket_count) {
  uint64_t h = 0;
  for (int i = 0; i < 8; i++) {
    h = (h << 8) | hash->bytes[i];
  }
  return h % bucket_count;
}

block_index_map_t *block_index_map_create(size_t initial_capacity) {
  block_index_map_t *map = malloc(sizeof(block_index_map_t));
  if (map == NULL) {
    return NULL;
  }

  if (initial_capacity == 0) {
    initial_capacity = BLOCK_INDEX_MAP_DEFAULT_SIZE;
  }

  map->bucket_count = initial_capacity;
  map->count = 0;
  map->buckets = calloc(initial_capacity, sizeof(block_index_t *));
  if (map->buckets == NULL) {
    free(map);
    return NULL;
  }

  return map;
}

void block_index_map_destroy(block_index_map_t *map) {
  if (map == NULL) {
    return;
  }

  /* Free all block indices (open addressing - one per bucket) */
  for (size_t i = 0; i < map->bucket_count; i++) {
    if (map->buckets[i] != NULL) {
      block_index_destroy(map->buckets[i]);
    }
  }

  free(map->buckets);
  free(map);
}

echo_result_t block_index_map_insert(block_index_map_t *map,
                                     block_index_t *index) {
  ECHO_ASSERT(map != NULL);
  ECHO_ASSERT(index != NULL);

  size_t bucket = block_index_map_hash(&index->hash, map->bucket_count);

  /* Linear probing for collision resolution */
  size_t original_bucket = bucket;
  do {
    if (map->buckets[bucket] == NULL) {
      /* Empty slot found */
      map->buckets[bucket] = index;
      map->count++;
      return ECHO_OK;
    }

    /* Check if already exists */
    if (memcmp(map->buckets[bucket]->hash.bytes, index->hash.bytes, 32) == 0) {
      return ECHO_ERR_EXISTS;
    }

    bucket = (bucket + 1) % map->bucket_count;
  } while (bucket != original_bucket);

  /* Table full - this shouldn't happen with proper sizing */
  return ECHO_ERR_NOMEM;
}

block_index_t *block_index_map_lookup(const block_index_map_t *map,
                                      const hash256_t *hash) {
  ECHO_ASSERT(map != NULL);
  ECHO_ASSERT(hash != NULL);

  size_t bucket = block_index_map_hash(hash, map->bucket_count);
  size_t original_bucket = bucket;

  do {
    if (map->buckets[bucket] == NULL) {
      return NULL; /* Empty slot - not found */
    }

    if (memcmp(map->buckets[bucket]->hash.bytes, hash->bytes, 32) == 0) {
      return map->buckets[bucket];
    }

    bucket = (bucket + 1) % map->bucket_count;
  } while (bucket != original_bucket);

  return NULL;
}

size_t block_index_map_size(const block_index_map_t *map) {
  ECHO_ASSERT(map != NULL);
  return map->count;
}

block_index_t *block_index_map_find_best(const block_index_map_t *map) {
  ECHO_ASSERT(map != NULL);

  block_index_t *best = NULL;

  for (size_t i = 0; i < map->bucket_count; i++) {
    block_index_t *index = map->buckets[i];
    if (index == NULL) {
      continue;
    }

    if (best == NULL ||
        work256_compare(&index->chainwork, &best->chainwork) > 0) {
      best = index;
    }
  }

  return best;
}

/* ========================================================================
 * Chain Selection Implementation (Session 6.3)
 * ======================================================================== */

chain_compare_result_t chain_compare(const block_index_t *a,
                                     const block_index_t *b) {
  ECHO_ASSERT(a != NULL);
  ECHO_ASSERT(b != NULL);

  int cmp = work256_compare(&a->chainwork, &b->chainwork);

  if (cmp > 0) {
    return CHAIN_COMPARE_A_BETTER;
  } else if (cmp < 0) {
    return CHAIN_COMPARE_B_BETTER;
  } else {
    /* Equal work - prefer status quo (A) */
    return CHAIN_COMPARE_EQUAL;
  }
}

block_index_t *chain_find_common_ancestor(block_index_t *a, block_index_t *b) {
  ECHO_ASSERT(a != NULL);
  ECHO_ASSERT(b != NULL);

  /* Walk both chains back to the same height */
  block_index_t *pa = a;
  block_index_t *pb = b;

  /* First, bring both pointers to the same height */
  while (pa->height > pb->height && pa->prev != NULL) {
    pa = pa->prev;
  }
  while (pb->height > pa->height && pb->prev != NULL) {
    pb = pb->prev;
  }

  /* Now walk back together until we find the common ancestor */
  while (pa != NULL && pb != NULL) {
    if (memcmp(pa->hash.bytes, pb->hash.bytes, 32) == 0) {
      return pa; /* Found common ancestor */
    }

    pa = pa->prev;
    pb = pb->prev;
  }

  /* No common ancestor (shouldn't happen on a valid chain) */
  return NULL;
}

chain_reorg_t *chain_reorg_create(block_index_t *current,
                                  block_index_t *new_tip) {
  ECHO_ASSERT(current != NULL);
  ECHO_ASSERT(new_tip != NULL);

  /* Find common ancestor */
  block_index_t *ancestor = chain_find_common_ancestor(current, new_tip);
  if (ancestor == NULL) {
    return NULL; /* Chains don't share history */
  }

  chain_reorg_t *reorg = malloc(sizeof(chain_reorg_t));
  if (reorg == NULL) {
    return NULL;
  }

  reorg->ancestor = ancestor;

  /* Count blocks to disconnect (from current tip to ancestor) */
  size_t disconnect_count = 0;
  block_index_t *p = current;
  while (p != NULL && memcmp(p->hash.bytes, ancestor->hash.bytes, 32) != 0) {
    disconnect_count++;
    p = p->prev;
  }

  /* Count blocks to connect (from ancestor to new tip) */
  size_t connect_count = 0;
  p = new_tip;
  while (p != NULL && memcmp(p->hash.bytes, ancestor->hash.bytes, 32) != 0) {
    connect_count++;
    p = p->prev;
  }

  /* Allocate arrays */
  reorg->disconnect_count = disconnect_count;
  reorg->connect_count = connect_count;

  if (disconnect_count > 0) {
    reorg->disconnect = malloc(disconnect_count * sizeof(block_index_t *));
    if (reorg->disconnect == NULL) {
      free(reorg);
      return NULL;
    }

    /* Fill disconnect array (tip to ancestor order) */
    p = current;
    for (size_t i = 0; i < disconnect_count; i++) {
      reorg->disconnect[i] = p;
      p = p->prev;
    }
  } else {
    reorg->disconnect = NULL;
  }

  if (connect_count > 0) {
    reorg->connect = malloc(connect_count * sizeof(block_index_t *));
    if (reorg->connect == NULL) {
      free(reorg->disconnect);
      free(reorg);
      return NULL;
    }

    /* Fill connect array in reverse order (ancestor to new tip) */
    p = new_tip;
    for (size_t i = connect_count; i > 0; i--) {
      reorg->connect[i - 1] = p;
      p = p->prev;
    }
  } else {
    reorg->connect = NULL;
  }

  return reorg;
}

void chain_reorg_destroy(chain_reorg_t *reorg) {
  if (reorg == NULL) {
    return;
  }

  free(reorg->disconnect);
  free(reorg->connect);
  free(reorg);
}

echo_result_t chain_reorganize(chainstate_t *state, chain_reorg_t *reorg,
                               get_block_txs_fn get_block_txs,
                               void *user_data) {
  ECHO_ASSERT(state != NULL);
  ECHO_ASSERT(reorg != NULL);
  ECHO_ASSERT(get_block_txs != NULL);

  echo_result_t result;

  /* Phase 1: Disconnect blocks from current chain */
  for (size_t i = 0; i < reorg->disconnect_count; i++) {
    block_index_t *to_disconnect = reorg->disconnect[i];

    /* Get the delta for this block */
    if (to_disconnect->height >= state->deltas_capacity ||
        state->deltas[to_disconnect->height] == NULL) {
      /* No undo data available - critical error */
      return ECHO_ERR_NOT_FOUND;
    }

    block_delta_t *delta = state->deltas[to_disconnect->height];

    /* Verify we're reverting the right block */
    if (memcmp(state->tip.hash.bytes, to_disconnect->hash.bytes, 32) != 0) {
      return ECHO_ERR_INVALID_BLOCK;
    }

    /* Revert the block */
    result = chainstate_revert_block(state, delta);
    if (result != ECHO_OK) {
      return result;
    }

    /* Mark block as no longer on main chain */
    to_disconnect->on_main_chain = false;

    /* Clear the delta */
    block_delta_destroy(delta);
    state->deltas[to_disconnect->height] = NULL;
  }

  /* Phase 2: Connect blocks on new chain */
  for (size_t i = 0; i < reorg->connect_count; i++) {
    block_index_t *to_connect = reorg->connect[i];

    /* Get transaction data for this block */
    const tx_t *txs = NULL;
    size_t tx_count = 0;
    result = get_block_txs(&to_connect->hash, &txs, &tx_count, user_data);
    if (result != ECHO_OK) {
      return result;
    }

    /* We need to construct a block header from the index */
    /* For now, we'll create a minimal header with the info we have */
    block_header_t header;
    header.version = 1; /* Simplified - real impl would store this */
    header.prev_hash = to_connect->prev_hash;
    memset(&header.merkle_root, 0, 32); /* Simplified */
    header.timestamp = to_connect->timestamp;
    header.bits = to_connect->bits;
    header.nonce = 0; /* Not needed for application */

    /* Apply the block */
    block_delta_t *delta = NULL;
    result = chainstate_apply_block(state, &header, txs, tx_count, &delta);
    if (result != ECHO_OK) {
      return result;
    }

    /* Store delta for future undo */
    if (delta != NULL) {
      /* Ensure capacity */
      if (to_connect->height >= state->deltas_capacity) {
        size_t new_cap = state->deltas_capacity * 2;
        while (new_cap <= to_connect->height) {
          new_cap *= 2;
        }
        block_delta_t **new_deltas =
            realloc(state->deltas, new_cap * sizeof(block_delta_t *));
        if (new_deltas == NULL) {
          block_delta_destroy(delta);
          return ECHO_ERR_NOMEM;
        }
        memset(new_deltas + state->deltas_capacity, 0,
               (new_cap - state->deltas_capacity) * sizeof(block_delta_t *));
        state->deltas = new_deltas;
        state->deltas_capacity = new_cap;
      }
      state->deltas[to_connect->height] = delta;
    }

    /* Mark block as on main chain */
    to_connect->on_main_chain = true;
  }

  /* Update tip index */
  if (reorg->connect_count > 0) {
    state->tip_index = reorg->connect[reorg->connect_count - 1];
  } else if (reorg->ancestor != NULL) {
    state->tip_index = reorg->ancestor;
  }

  return ECHO_OK;
}

echo_result_t chainstate_add_header_with_hash(chainstate_t *state,
                                              const block_header_t *header,
                                              const hash256_t *precomputed_hash,
                                              block_index_t **index_out) {
  ECHO_ASSERT(state != NULL);
  ECHO_ASSERT(header != NULL);

  /* Use pre-computed hash if provided, otherwise compute */
  hash256_t local_hash;
  const hash256_t *hash_ptr;

  if (precomputed_hash != NULL) {
    hash_ptr = precomputed_hash;
  } else {
    echo_result_t result = block_header_hash(header, &local_hash);
    if (result != ECHO_OK) {
      return result;
    }
    hash_ptr = &local_hash;
  }

  /* Check if already known */
  if (block_index_map_lookup(state->block_map, hash_ptr) != NULL) {
    return ECHO_ERR_EXISTS;
  }

  /* Find parent block index if not genesis */
  block_index_t *prev_index = NULL;
  bool is_genesis = true;
  for (int i = 0; i < 32; i++) {
    if (header->prev_hash.bytes[i] != 0) {
      is_genesis = false;
      break;
    }
  }

  if (!is_genesis) {
    prev_index = block_index_map_lookup(state->block_map, &header->prev_hash);
    /* Note: prev_index may be NULL for orphan blocks */
  }

  /* Create block index */
  block_index_t *index = block_index_create(header, prev_index);
  if (index == NULL) {
    return ECHO_ERR_NOMEM;
  }

  /* Insert into map */
  echo_result_t result = block_index_map_insert(state->block_map, index);
  if (result != ECHO_OK) {
    block_index_destroy(index);
    return result;
  }

  if (index_out != NULL) {
    *index_out = index;
  }

  return ECHO_OK;
}

echo_result_t chainstate_add_header(chainstate_t *state,
                                    const block_header_t *header,
                                    block_index_t **index_out) {
  return chainstate_add_header_with_hash(state, header, NULL, index_out);
}

block_index_map_t *chainstate_get_block_index_map(chainstate_t *state) {
  ECHO_ASSERT(state != NULL);
  return state->block_map;
}

block_index_t *chainstate_get_tip_index(const chainstate_t *state) {
  ECHO_ASSERT(state != NULL);
  return state->tip_index;
}

void chainstate_set_tip_index(chainstate_t *state, block_index_t *index) {
  ECHO_ASSERT(state != NULL);
  state->tip_index = index;

  if (index != NULL) {
    state->tip.hash = index->hash;
    state->tip.height = index->height;
    state->tip.chainwork = index->chainwork;

    /*
     * Rebuild height_index by walking backwards from tip.
     *
     * This is needed when restoring from a checkpoint, where we set the tip
     * directly without having applied blocks one by one (which normally
     * populates height_index incrementally via chainstate_apply_block).
     *
     * Without this, consensus_build_validation_ctx fails to look up
     * timestamps for MTP calculation, causing blocks to fail validation.
     */
    if (index->height + 1 > state->height_index_capacity) {
      /* Resize height_index if needed */
      size_t new_capacity = index->height + 1024;
      hash256_t *new_index =
          realloc(state->height_index, new_capacity * sizeof(hash256_t));
      if (new_index != NULL) {
        state->height_index = new_index;
        state->height_index_capacity = new_capacity;
      }
    }

    /* Walk backwards from tip and populate height_index */
    block_index_t *walk = index;
    while (walk != NULL && walk->height < state->height_index_capacity) {
      state->height_index[walk->height] = walk->hash;
      walk = walk->prev;
    }
  }
}

void chainstate_set_best_header_index(chainstate_t *state,
                                      block_index_t *index) {
  ECHO_ASSERT(state != NULL);
  /* Only set tip_index (for sync locator building).
   * Does NOT update state->tip - that remains the validated chain tip. */
  state->tip_index = index;
}

bool chainstate_should_reorg(const chainstate_t *state,
                             const block_index_t *new_index) {
  ECHO_ASSERT(state != NULL);
  ECHO_ASSERT(new_index != NULL);

  if (state->tip_index == NULL) {
    /* No current tip - any block is better */
    return true;
  }

  /* Compare by accumulated work */
  return chain_compare(new_index, state->tip_index) == CHAIN_COMPARE_A_BETTER;
}
