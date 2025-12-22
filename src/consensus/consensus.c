/**
 * @file consensus.c
 * @brief Consensus engine implementation
 *
 * This file implements the unified Bitcoin consensus engine interface.
 * The consensus engine integrates:
 *   - Block validation (header, structure, merkle root)
 *   - Transaction validation (scripts, signatures)
 *   - Chain state management (UTXO set, chain tip)
 *   - Chain selection (most work wins)
 *   - Reorganization handling
 *
 * Key design principles:
 *   - Pure functions where possible (validation doesn't modify state)
 *   - Clear separation between validation and state modification
 *   - No system calls in the consensus path
 *
 * Build once. Build right. Stop.
 */

#include "consensus.h"
#include "block.h"
#include "block_validate.h"
#include "chainstate.h"
#include "echo_assert.h"
#include "echo_config.h"
#include "echo_types.h"
#include "script.h"
#include "tx.h"
#include "tx_validate.h"
#include "utxo.h"

/* IBD profiling - Session 9.6.7+ Phase 2 */
#define LOG_COMPONENT LOG_COMP_CONS
#include "log.h"
#include "platform.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * ============================================================================
 * CONSENSUS ENGINE STRUCTURE
 * ============================================================================
 */

struct consensus_engine {
  chainstate_t *chainstate; /* Chain state (tip, UTXO set, block index) */
  bool initialized;         /* True if genesis has been processed */
};

/*
 * ============================================================================
 * ERROR STRINGS
 * ============================================================================
 */

static const char *consensus_error_strings[] = {
    [CONSENSUS_OK] = "OK",
    [CONSENSUS_ERR_BLOCK_HEADER] = "Block header validation failed",
    [CONSENSUS_ERR_BLOCK_POW] = "Proof of work failed",
    [CONSENSUS_ERR_BLOCK_TIMESTAMP] = "Block timestamp invalid",
    [CONSENSUS_ERR_BLOCK_DIFFICULTY] = "Difficulty mismatch",
    [CONSENSUS_ERR_BLOCK_SIZE] = "Block size/weight exceeded",
    [CONSENSUS_ERR_BLOCK_MERKLE] = "Merkle root mismatch",
    [CONSENSUS_ERR_BLOCK_NO_COINBASE] = "Missing coinbase transaction",
    [CONSENSUS_ERR_BLOCK_MULTI_COINBASE] = "Multiple coinbase transactions",
    [CONSENSUS_ERR_BLOCK_TX_ORDER] = "Coinbase not first transaction",
    [CONSENSUS_ERR_BLOCK_DUPLICATE_TX] = "Duplicate transaction",
    [CONSENSUS_ERR_BLOCK_WITNESS] = "Witness commitment invalid",
    [CONSENSUS_ERR_BLOCK_COINBASE] = "Coinbase validation failed",
    [CONSENSUS_ERR_TX_SYNTAX] = "Transaction syntax error",
    [CONSENSUS_ERR_TX_SCRIPT] = "Script execution failed",
    [CONSENSUS_ERR_TX_MISSING_INPUT] = "Missing input UTXO",
    [CONSENSUS_ERR_TX_SPENT_INPUT] = "Input already spent",
    [CONSENSUS_ERR_TX_IMMATURE_COINBASE] = "Spending immature coinbase",
    [CONSENSUS_ERR_TX_VALUE_MISMATCH] = "Output value exceeds input value",
    [CONSENSUS_ERR_TX_LOCKTIME] = "Locktime not satisfied",
    [CONSENSUS_ERR_INVALID_PREV] = "Previous block unknown or invalid",
    [CONSENSUS_ERR_REORG_FAILED] = "Chain reorganization failed",
    [CONSENSUS_ERR_UTXO_CONFLICT] = "UTXO set inconsistency",
    [CONSENSUS_ERR_INTERNAL] = "Internal error",
    [CONSENSUS_ERR_NOMEM] = "Out of memory",
};

/*
 * ============================================================================
 * RESULT INITIALIZATION
 * ============================================================================
 */

void consensus_result_init(consensus_result_t *result) {
  ECHO_ASSERT(result != NULL);

  result->error = CONSENSUS_OK;
  result->failing_index = 0;
  result->failing_input_index = 0;
  result->block_error = BLOCK_VALID;
  result->tx_error = TX_VALIDATE_OK;
  result->script_error = SCRIPT_ERR_OK;
}

const char *consensus_error_str(consensus_error_t error) {
  if (error <
      sizeof(consensus_error_strings) / sizeof(consensus_error_strings[0])) {
    const char *str = consensus_error_strings[error];
    if (str != NULL) {
      return str;
    }
  }
  return "Unknown error";
}

/*
 * ============================================================================
 * ENGINE LIFECYCLE
 * ============================================================================
 */

consensus_engine_t *consensus_engine_create(void) {
  consensus_engine_t *engine = malloc(sizeof(consensus_engine_t));
  if (engine == NULL) {
    return NULL;
  }

  engine->chainstate = chainstate_create();
  if (engine->chainstate == NULL) {
    free(engine);
    return NULL;
  }

  engine->initialized = false;

  return engine;
}

void consensus_engine_destroy(consensus_engine_t *engine) {
  if (engine == NULL) {
    return;
  }

  chainstate_destroy(engine->chainstate);
  free(engine);
}

/*
 * ============================================================================
 * CHAIN TIP QUERIES
 * ============================================================================
 */

echo_result_t consensus_get_chain_tip(const consensus_engine_t *engine,
                                      chain_tip_t *tip) {
  ECHO_ASSERT(engine != NULL);
  ECHO_ASSERT(tip != NULL);

  return chainstate_get_tip(engine->chainstate, tip);
}

uint32_t consensus_get_height(const consensus_engine_t *engine) {
  ECHO_ASSERT(engine != NULL);

  if (!engine->initialized) {
    return UINT32_MAX; /* No blocks yet */
  }

  return chainstate_get_height(engine->chainstate);
}

void consensus_mark_initialized(consensus_engine_t *engine) {
  ECHO_ASSERT(engine != NULL);
  engine->initialized = true;
}

echo_result_t consensus_get_block_hash(const consensus_engine_t *engine,
                                       uint32_t height, hash256_t *hash) {
  ECHO_ASSERT(engine != NULL);
  ECHO_ASSERT(hash != NULL);

  return chainstate_get_block_at_height(engine->chainstate, height, hash);
}

bool consensus_is_main_chain(const consensus_engine_t *engine,
                             const hash256_t *hash) {
  ECHO_ASSERT(engine != NULL);
  ECHO_ASSERT(hash != NULL);

  return chainstate_is_on_main_chain(engine->chainstate, hash);
}

/*
 * ============================================================================
 * UTXO QUERIES
 * ============================================================================
 */

const utxo_entry_t *consensus_lookup_utxo(const consensus_engine_t *engine,
                                          const outpoint_t *outpoint) {
  ECHO_ASSERT(engine != NULL);
  ECHO_ASSERT(outpoint != NULL);

  return chainstate_lookup_utxo(engine->chainstate, outpoint);
}

bool consensus_utxo_exists(const consensus_engine_t *engine,
                           const outpoint_t *outpoint) {
  return consensus_lookup_utxo(engine, outpoint) != NULL;
}

size_t consensus_utxo_count(const consensus_engine_t *engine) {
  ECHO_ASSERT(engine != NULL);

  const utxo_set_t *utxo_set = chainstate_get_utxo_set(engine->chainstate);
  return utxo_set_size(utxo_set);
}

/*
 * ============================================================================
 * BLOCK INDEX QUERIES
 * ============================================================================
 */

const block_index_t *
consensus_lookup_block_index(const consensus_engine_t *engine,
                             const hash256_t *hash) {
  ECHO_ASSERT(engine != NULL);
  ECHO_ASSERT(hash != NULL);

  block_index_map_t *map = chainstate_get_block_index_map(engine->chainstate);
  return block_index_map_lookup(map, hash);
}

size_t consensus_block_index_count(const consensus_engine_t *engine) {
  ECHO_ASSERT(engine != NULL);

  block_index_map_t *map = chainstate_get_block_index_map(engine->chainstate);
  return block_index_map_size(map);
}

const block_index_t *
consensus_get_best_block_index(const consensus_engine_t *engine) {
  ECHO_ASSERT(engine != NULL);

  block_index_map_t *map = chainstate_get_block_index_map(engine->chainstate);
  return block_index_map_find_best(map);
}

/*
 * ============================================================================
 * SCRIPT VERIFICATION FLAGS
 * ============================================================================
 */

uint32_t consensus_get_script_flags(uint32_t height) {
  uint32_t flags = SCRIPT_VERIFY_NONE;

  /* BIP-16: P2SH */
  if (height >= CONSENSUS_BIP16_HEIGHT) {
    flags |= SCRIPT_VERIFY_P2SH;
  }

  /* BIP-66: Strict DER signatures */
  if (height >= CONSENSUS_BIP66_HEIGHT) {
    flags |= SCRIPT_VERIFY_DERSIG;
  }

  /* BIP-65: OP_CHECKLOCKTIMEVERIFY */
  if (height >= CONSENSUS_BIP65_HEIGHT) {
    flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
  }

  /* BIP-68/112/113: Relative locktimes */
  if (height >= CONSENSUS_CSV_HEIGHT) {
    flags |= SCRIPT_VERIFY_CHECKSEQUENCEVERIFY;
  }

  /* BIP-141: SegWit */
  if (height >= CONSENSUS_SEGWIT_HEIGHT) {
    flags |= SCRIPT_VERIFY_WITNESS;
  }

  /* BIP-341: Taproot */
  if (height >= CONSENSUS_TAPROOT_HEIGHT) {
    flags |= SCRIPT_VERIFY_TAPROOT;
  }

  return flags;
}

/*
 * ============================================================================
 * VALIDATION CONTEXT BUILDING
 * ============================================================================
 */

echo_result_t consensus_build_validation_ctx(const consensus_engine_t *engine,
                                             uint32_t height,
                                             full_block_ctx_t *ctx) {
  ECHO_ASSERT(engine != NULL);
  ECHO_ASSERT(ctx != NULL);

  full_block_ctx_init(ctx);

  ctx->height = height;
  ctx->header_ctx.height = height;

  /* Get parent information */
  if (height > 0) {
    hash256_t parent_hash;
    echo_result_t result = chainstate_get_block_at_height(
        engine->chainstate, height - 1, &parent_hash);

    if (result == ECHO_OK) {
      ctx->header_ctx.parent_hash = parent_hash;
      ctx->header_ctx.parent_valid = ECHO_TRUE;
    } else {
      ctx->header_ctx.parent_valid = ECHO_FALSE;
    }

    /* Build timestamp history for MTP */
    ctx->header_ctx.timestamp_count = 0;
    for (uint32_t i = 0; i < BLOCK_MTP_WINDOW && i < height; i++) {
      uint32_t h = height - 1 - i;
      hash256_t block_hash;
      if (chainstate_get_block_at_height(engine->chainstate, h, &block_hash) ==
          ECHO_OK) {
        const block_index_t *index =
            consensus_lookup_block_index(engine, &block_hash);
        if (index != NULL) {
          ctx->header_ctx.timestamps[i] = index->timestamp;
          ctx->header_ctx.timestamp_count++;
        }
      }
    }
  } else {
    /* Genesis block */
    memset(&ctx->header_ctx.parent_hash, 0, 32);
    ctx->header_ctx.parent_valid = ECHO_TRUE;
    ctx->header_ctx.timestamp_count = 0;
  }

  /* Set SegWit status */
  ctx->segwit_active =
      (height >= CONSENSUS_SEGWIT_HEIGHT) ? ECHO_TRUE : ECHO_FALSE;

  /* Difficulty context would be populated here based on chain history */
  difficulty_ctx_init(&ctx->difficulty_ctx);
  ctx->difficulty_ctx.height = height;

  return ECHO_OK;
}

echo_result_t consensus_build_tx_ctx(const consensus_engine_t *engine,
                                     const tx_t *tx, uint32_t block_height,
                                     uint32_t block_time,
                                     tx_validate_ctx_t *ctx) {
  ECHO_ASSERT(engine != NULL);
  ECHO_ASSERT(tx != NULL);
  ECHO_ASSERT(ctx != NULL);

  /* Initialize context */
  memset(ctx, 0, sizeof(tx_validate_ctx_t));
  ctx->block_height = block_height;
  ctx->block_time = block_time;
  ctx->script_flags = consensus_get_script_flags(block_height);

  /* Allocate UTXO info array */
  if (tx->input_count > 0) {
    utxo_info_t *utxos = calloc(tx->input_count, sizeof(utxo_info_t));
    if (utxos == NULL) {
      return ECHO_ERR_NOMEM;
    }
    ctx->utxos = utxos;
    ctx->utxo_count = tx->input_count;

    /* Populate UTXO info for each input */
    for (size_t i = 0; i < tx->input_count; i++) {
      const outpoint_t *outpoint = &tx->inputs[i].prevout;
      const utxo_entry_t *utxo = consensus_lookup_utxo(engine, outpoint);

      if (utxo == NULL) {
        /* UTXO not found */
        free(utxos);
        ctx->utxos = NULL;
        return ECHO_ERR_NOT_FOUND;
      }

      /* Populate UTXO info */
      utxos[i].value = utxo->value;
      utxos[i].script_pubkey = utxo->script_pubkey;
      utxos[i].script_pubkey_len = utxo->script_len;
      utxos[i].height = utxo->height;
      utxos[i].is_coinbase = utxo->is_coinbase ? ECHO_TRUE : ECHO_FALSE;
    }
  }

  return ECHO_OK;
}

void consensus_free_tx_ctx(tx_validate_ctx_t *ctx) {
  if (ctx == NULL) {
    return;
  }

  /* Note: We don't free script_pubkey pointers because they point into
   * the UTXO set, which owns that memory */
  /* Cast away const for free() - safe because free() doesn't modify memory */
  union {
    const utxo_info_t *c;
    utxo_info_t *nc;
  } u = {.c = ctx->utxos};
  free(u.nc);
  ctx->utxos = NULL;
  ctx->utxo_count = 0;
}

/*
 * ============================================================================
 * PURE VALIDATION FUNCTIONS
 * ============================================================================
 */

/**
 * Check if a block header connects to our chain.
 */
static bool header_connects(const consensus_engine_t *engine,
                            const block_header_t *header) {
  /* Check for genesis (prev_hash all zeros) */
  bool is_genesis = true;
  for (int i = 0; i < 32; i++) {
    if (header->prev_hash.bytes[i] != 0) {
      is_genesis = false;
      break;
    }
  }

  if (is_genesis) {
    /* Genesis block must come first */
    return !engine->initialized;
  }

  /* Check if parent exists in our index */
  return consensus_lookup_block_index(engine, &header->prev_hash) != NULL;
}

/**
 * Get the height for a new block given its header.
 */
static uint32_t get_block_height(const consensus_engine_t *engine,
                                 const block_header_t *header) {
  /* Check for genesis */
  bool is_genesis = true;
  for (int i = 0; i < 32; i++) {
    if (header->prev_hash.bytes[i] != 0) {
      is_genesis = false;
      break;
    }
  }

  if (is_genesis) {
    return 0;
  }

  const block_index_t *parent =
      consensus_lookup_block_index(engine, &header->prev_hash);
  if (parent == NULL) {
    return UINT32_MAX; /* Unknown parent */
  }

  return parent->height + 1;
}

bool consensus_validate_header_with_hash(const consensus_engine_t *engine,
                                         const block_header_t *header,
                                         const hash256_t *precomputed_hash,
                                         consensus_result_t *result) {
  ECHO_ASSERT(engine != NULL);
  ECHO_ASSERT(header != NULL);

  if (result != NULL) {
    consensus_result_init(result);
  }

  /* Check if header connects to our chain */
  if (!header_connects(engine, header)) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_INVALID_PREV;
    }
    return false;
  }

  /* Validate proof of work (use pre-computed hash if available) */
  block_validation_error_t pow_error;
  if (!block_validate_pow_with_hash(header, precomputed_hash, &pow_error)) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_BLOCK_POW;
      result->block_error = pow_error;
    }
    return false;
  }

  /* Get block height for context */
  uint32_t height = get_block_height(engine, header);
  if (height == UINT32_MAX) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_INVALID_PREV;
    }
    return false;
  }

  /* Build validation context */
  full_block_ctx_t ctx;
  consensus_build_validation_ctx(engine, height, &ctx);

  /* Validate timestamp */
  block_validation_error_t ts_error;
  if (!block_validate_timestamp(header, &ctx.header_ctx, &ts_error)) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_BLOCK_TIMESTAMP;
      result->block_error = ts_error;
    }
    return false;
  }

  /* Validate version */
  block_validation_error_t ver_error;
  if (!block_validate_version(header, &ctx.header_ctx, &ver_error)) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_BLOCK_HEADER;
      result->block_error = ver_error;
    }
    return false;
  }

  return true;
}

bool consensus_validate_header(const consensus_engine_t *engine,
                               const block_header_t *header,
                               consensus_result_t *result) {
  return consensus_validate_header_with_hash(engine, header, NULL, result);
}

/*
 * Internal block validation with optional TXID output.
 *
 * If txids_out is non-NULL, the computed TXIDs are returned to the caller
 * who becomes responsible for freeing them. This allows the TXIDs to be
 * reused for block application, avoiding redundant computation.
 */
static bool validate_block_internal(const consensus_engine_t *engine,
                                    const block_t *block,
                                    hash256_t **txids_out,
                                    consensus_result_t *result) {
  ECHO_ASSERT(engine != NULL);
  ECHO_ASSERT(block != NULL);

  /* IBD profiling - track validation timing */
  uint64_t block_start = plat_monotonic_ms();
  uint64_t script_time_total = 0;
  uint64_t sameblock_time_total = 0;
  size_t total_inputs = 0;
  size_t scripts_verified = 0;
  size_t sameblock_lookups = 0;

  if (result != NULL) {
    consensus_result_init(result);
  }

  /* First validate the header */
  if (!consensus_validate_header(engine, &block->header, result)) {
    return false;
  }

  uint32_t height = get_block_height(engine, &block->header);

  /* Check block has transactions */
  if (block->tx_count == 0) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_BLOCK_NO_COINBASE;
    }
    return false;
  }

  /* Check first transaction is coinbase */
  if (!tx_is_coinbase(&block->txs[0])) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_BLOCK_NO_COINBASE;
    }
    return false;
  }

  /* Check no other transactions are coinbase */
  for (size_t i = 1; i < block->tx_count; i++) {
    if (tx_is_coinbase(&block->txs[i])) {
      if (result != NULL) {
        result->error = CONSENSUS_ERR_BLOCK_MULTI_COINBASE;
        result->failing_index = i;
      }
      return false;
    }
  }

  /*
   * PRE-COMPUTE ALL TXIDS ONCE
   *
   * TXIDs are needed for: merkle root, duplicate check, same-block deps, UTXO creation.
   * By computing once upfront, we eliminate 2x redundant SHA256d per transaction.
   * For a 1000-tx block, this saves ~2000 SHA256d operations.
   */
  hash256_t *block_txids = NULL;
  if (block->tx_count > 0) {
    block_txids = malloc(block->tx_count * sizeof(hash256_t));
    if (block_txids != NULL) {
      for (size_t i = 0; i < block->tx_count; i++) {
        tx_compute_txid(&block->txs[i], &block_txids[i]);
      }
    }
  }

  /* Check for duplicate txids */
  size_t dup_idx;
  if (block_has_duplicate_txids(block, &dup_idx)) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_BLOCK_DUPLICATE_TX;
      result->failing_index = dup_idx;
    }
    free(block_txids);
    return false;
  }

  /* Verify merkle root using pre-computed TXIDs (avoids recomputing them) */
  block_validation_error_t merkle_error;
  if (!block_validate_merkle_root_with_txids(block, block_txids, &merkle_error)) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_BLOCK_MERKLE;
      result->block_error = merkle_error;
    }
    free(block_txids);
    return false;
  }

  /* Validate block size/weight */
  block_validation_error_t size_error;
  if (!block_validate_size(block, &size_error)) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_BLOCK_SIZE;
      result->block_error = size_error;
    }
    free(block_txids);
    return false;
  }

  /* Calculate total fees (sum of non-coinbase tx fees) */
  satoshi_t total_fees = 0;

  /* Validate all non-coinbase transactions */
  uint32_t script_flags = consensus_get_script_flags(height);

  /*
   * Full Verification: Bitcoin Echo verifies every signature.
   * Per the manifesto: "Verify, not believe."
   */

  for (size_t tx_idx = 1; tx_idx < block->tx_count; tx_idx++) {
    const tx_t *tx = &block->txs[tx_idx];

    /* Check syntax */
    tx_validate_result_t tx_result;
    tx_validate_result_init(&tx_result);

    if (tx_validate_syntax(tx, &tx_result) != ECHO_OK) {
      if (result != NULL) {
        result->error = CONSENSUS_ERR_TX_SYNTAX;
        result->failing_index = tx_idx;
        result->tx_error = tx_result.error;
      }
      free(block_txids);
      return false;
    }

    /* Validate each input against UTXO set */
    satoshi_t tx_input_total = 0;

    for (size_t in_idx = 0; in_idx < tx->input_count; in_idx++) {
      const outpoint_t *outpoint = &tx->inputs[in_idx].prevout;
      const utxo_entry_t *utxo = consensus_lookup_utxo(engine, outpoint);

      /* UTXO info for script validation */
      utxo_info_t utxo_info;
      memset(&utxo_info, 0, sizeof(utxo_info));

      if (utxo == NULL) {
        /* Check if this input references an output from earlier in this block.
         * Use pre-computed txids for O(n) scan with memcmp only (no SHA256d).
         */
        uint64_t sameblock_start = plat_monotonic_ms();
        bool found_in_block = false;
        for (size_t prev_tx = 0; prev_tx < tx_idx; prev_tx++) {
          /* Use pre-computed txid if available, else compute on the fly */
          const hash256_t *prev_txid_ptr;
          hash256_t prev_txid_computed;
          if (block_txids != NULL) {
            prev_txid_ptr = &block_txids[prev_tx];
          } else {
            tx_compute_txid(&block->txs[prev_tx], &prev_txid_computed);
            prev_txid_ptr = &prev_txid_computed;
          }
          if (memcmp(outpoint->txid.bytes, prev_txid_ptr->bytes, 32) == 0 &&
              outpoint->vout < block->txs[prev_tx].output_count) {
            /* Found it in this block */
            const tx_output_t *prev_out =
                &block->txs[prev_tx].outputs[outpoint->vout];
            tx_input_total += prev_out->value;
            found_in_block = true;

            /* Build UTXO info for script validation */
            utxo_info.value = prev_out->value;
            utxo_info.script_pubkey = prev_out->script_pubkey;
            utxo_info.script_pubkey_len = prev_out->script_pubkey_len;
            utxo_info.height = height; /* Same block */
            utxo_info.is_coinbase = (prev_tx == 0) ? ECHO_TRUE : ECHO_FALSE;
            break;
          }
        }
        sameblock_time_total += plat_monotonic_ms() - sameblock_start;
        sameblock_lookups++;

        if (!found_in_block) {
          if (result != NULL) {
            result->error = CONSENSUS_ERR_TX_MISSING_INPUT;
            result->failing_index = tx_idx;
            result->failing_input_index = in_idx;
          }
          free(block_txids);
          return false;
        }
      } else {
        /* Check coinbase maturity */
        if (utxo->is_coinbase) {
          if (!utxo_entry_is_mature(utxo, height)) {
            if (result != NULL) {
              result->error = CONSENSUS_ERR_TX_IMMATURE_COINBASE;
              result->failing_index = tx_idx;
              result->failing_input_index = in_idx;
            }
            free(block_txids);
            return false;
          }
        }

        tx_input_total += utxo->value;

        /* Build UTXO info for script validation */
        utxo_info.value = utxo->value;
        utxo_info.script_pubkey = utxo->script_pubkey;
        utxo_info.script_pubkey_len = utxo->script_len;
        utxo_info.height = utxo->height;
        utxo_info.is_coinbase = utxo->is_coinbase ? ECHO_TRUE : ECHO_FALSE;
      }

      /* Script validation - verify every signature */
      uint64_t script_start = plat_monotonic_ms();
      tx_validate_result_t script_result;
      tx_validate_result_init(&script_result);

      echo_result_t script_res =
          tx_validate_input(tx, in_idx, &utxo_info, script_flags,
                            &script_result);
      script_time_total += plat_monotonic_ms() - script_start;
      scripts_verified++;

      if (script_res != ECHO_OK) {
        if (result != NULL) {
          result->error = CONSENSUS_ERR_TX_SCRIPT;
          result->failing_index = tx_idx;
          result->failing_input_index = in_idx;
          result->script_error = script_result.script_error;
        }
        free(block_txids);
        return false;
      }
      total_inputs++;
    }

    /* Calculate output total */
    satoshi_t tx_output_total = 0;
    for (size_t out_idx = 0; out_idx < tx->output_count; out_idx++) {
      tx_output_total += tx->outputs[out_idx].value;
    }

    /* Check inputs >= outputs */
    if (tx_input_total < tx_output_total) {
      if (result != NULL) {
        result->error = CONSENSUS_ERR_TX_VALUE_MISMATCH;
        result->failing_index = tx_idx;
      }
      free(block_txids);
      return false;
    }

    total_fees += (tx_input_total - tx_output_total);
  }

  /* Validate coinbase */
  satoshi_t subsidy = coinbase_subsidy(height);
  satoshi_t max_coinbase_value = subsidy + total_fees;

  block_validation_error_t cb_error;
  if (!coinbase_validate(&block->txs[0], height, max_coinbase_value,
                         &cb_error)) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_BLOCK_COINBASE;
      result->block_error = cb_error;
    }
    free(block_txids);
    return false;
  }

  /* Validate witness commitment if SegWit active */
  if (height >= CONSENSUS_SEGWIT_HEIGHT) {
    block_validation_error_t wit_error;
    if (!block_validate_witness_commitment(block, &wit_error)) {
      if (result != NULL) {
        result->error = CONSENSUS_ERR_BLOCK_WITNESS;
        result->block_error = wit_error;
      }
      free(block_txids);
      return false;
    }
  }

  /* IBD profiling - log validation timing */
  uint64_t block_elapsed = plat_monotonic_ms() - block_start;

  /* Only log detailed timing for blocks that take >10ms or every 1000 blocks */
  if (block_elapsed > 10 || height % 1000 == 0) {
    log_info(LOG_COMP_CONS,
             "Block %u validated in %lums (%zu txs, %zu inputs, "
             "scripts=%lums/%zu, sameblock=%lums/%zu)",
             height, (unsigned long)block_elapsed, block->tx_count,
             total_inputs, (unsigned long)script_time_total, scripts_verified,
             (unsigned long)sameblock_time_total, sameblock_lookups);
  }

  /* Return TXIDs to caller if requested, otherwise free them */
  if (txids_out != NULL) {
    *txids_out = block_txids;
  } else {
    free(block_txids);
  }
  return true;
}

bool consensus_validate_block(const consensus_engine_t *engine,
                              const block_t *block,
                              consensus_result_t *result) {
  return validate_block_internal(engine, block, NULL, result);
}

bool consensus_validate_tx(const consensus_engine_t *engine, const tx_t *tx,
                           uint32_t block_height, uint32_t block_time,
                           consensus_result_t *result) {
  ECHO_ASSERT(engine != NULL);
  ECHO_ASSERT(tx != NULL);

  if (result != NULL) {
    consensus_result_init(result);
  }

  /* Build transaction validation context */
  tx_validate_ctx_t ctx;
  echo_result_t build_result =
      consensus_build_tx_ctx(engine, tx, block_height, block_time, &ctx);

  if (build_result == ECHO_ERR_NOT_FOUND) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_TX_MISSING_INPUT;
    }
    return false;
  }

  if (build_result != ECHO_OK) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_INTERNAL;
    }
    return false;
  }

  /* Validate transaction */
  tx_validate_result_t tx_result;
  tx_validate_result_init(&tx_result);

  echo_result_t validate_result = tx_validate(tx, &ctx, &tx_result);
  consensus_free_tx_ctx(&ctx);

  if (validate_result != ECHO_OK) {
    if (result != NULL) {
      switch (tx_result.error) {
      case TX_VALIDATE_ERR_SCRIPT_FAILED:
      case TX_VALIDATE_ERR_SCRIPT_VERIFY:
        result->error = CONSENSUS_ERR_TX_SCRIPT;
        break;
      case TX_VALIDATE_ERR_MISSING_UTXO:
        result->error = CONSENSUS_ERR_TX_MISSING_INPUT;
        break;
      case TX_VALIDATE_ERR_LOCKTIME:
        result->error = CONSENSUS_ERR_TX_LOCKTIME;
        break;
      default:
        result->error = CONSENSUS_ERR_TX_SYNTAX;
        break;
      }
      result->tx_error = tx_result.error;
      result->failing_input_index = tx_result.index;
      result->script_error = tx_result.script_error;
    }
    return false;
  }

  return true;
}

/*
 * ============================================================================
 * STATE MODIFICATION FUNCTIONS
 * ============================================================================
 */

echo_result_t consensus_add_header(consensus_engine_t *engine,
                                   const block_header_t *header,
                                   block_index_t **index_out) {
  ECHO_ASSERT(engine != NULL);
  ECHO_ASSERT(header != NULL);

  return chainstate_add_header(engine->chainstate, header, index_out);
}

/*
 * Internal apply function that accepts optional pre-computed TXIDs.
 * If precomputed_txids is NULL, TXIDs are computed internally.
 */
static echo_result_t apply_block_internal(consensus_engine_t *engine,
                                          const block_t *block,
                                          const hash256_t *precomputed_txids,
                                          consensus_result_t *result) {
  ECHO_ASSERT(engine != NULL);
  ECHO_ASSERT(block != NULL);

  if (result != NULL) {
    consensus_result_init(result);
  }

  /* Add header to index if not already present */
  block_index_t *index = NULL;
  echo_result_t add_result =
      consensus_add_header(engine, &block->header, &index);
  if (add_result != ECHO_OK && add_result != ECHO_ERR_EXISTS) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_INTERNAL;
    }
    return add_result;
  }

  /* Use pre-computed TXIDs if provided, otherwise compute them */
  hash256_t *block_txids = NULL;
  bool owns_txids = false;
  if (precomputed_txids != NULL) {
    /* Cast away const - we won't modify, just pass through */
    block_txids = (hash256_t *)precomputed_txids;
  } else if (block->tx_count > 0) {
    block_txids = malloc(block->tx_count * sizeof(hash256_t));
    if (block_txids == NULL) {
      if (result != NULL) {
        result->error = CONSENSUS_ERR_INTERNAL;
      }
      return ECHO_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < block->tx_count; i++) {
      tx_compute_txid(&block->txs[i], &block_txids[i]);
    }
    owns_txids = true;
  }

  /*
   * Apply to chain state with pre-computed TXIDs.
   *
   * IBD optimization: Pass NULL for delta_out to skip undo data creation.
   * Creating deltas was O(n²) due to per-UTXO realloc - a massive bottleneck.
   * During IBD, reorgs of historical blocks are essentially impossible.
   * If we ever need to reorg during IBD, we'll restore from checkpoint.
   *
   * TODO: After IBD completes (near tip), enable delta tracking for shallow
   * reorgs. For now, we prioritize sync speed.
   */
  echo_result_t apply_result = chainstate_apply_block_with_txids(
      engine->chainstate, &block->header, block->txs, block->tx_count,
      block_txids, NULL); /* NULL = skip delta creation for IBD performance */

  if (owns_txids) {
    free(block_txids);
  }

  if (apply_result != ECHO_OK) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_UTXO_CONFLICT;
    }
    return apply_result;
  }

  /* Update tip index */
  if (index != NULL) {
    index->on_main_chain = true;
    chainstate_set_tip_index(engine->chainstate, index);
  }

  engine->initialized = true;

  return ECHO_OK;
}

echo_result_t consensus_apply_block(consensus_engine_t *engine,
                                    const block_t *block,
                                    consensus_result_t *result) {
  return apply_block_internal(engine, block, NULL, result);
}

echo_result_t consensus_validate_and_apply_block(consensus_engine_t *engine,
                                                  const block_t *block,
                                                  consensus_result_t *result) {
  ECHO_ASSERT(engine != NULL);
  ECHO_ASSERT(block != NULL);

  /* Validate block, getting TXIDs for reuse in apply */
  hash256_t *block_txids = NULL;
  bool valid = validate_block_internal(engine, block, &block_txids, result);

  if (!valid) {
    /* Validation failed - TXIDs were freed by validate_block_internal */
    return ECHO_ERR_INVALID;
  }

  /* Apply block using the same TXIDs (avoids recomputing) */
  echo_result_t apply_result =
      apply_block_internal(engine, block, block_txids, result);

  free(block_txids);

  return apply_result;
}

bool consensus_would_reorg(const consensus_engine_t *engine,
                           const block_header_t *header) {
  ECHO_ASSERT(engine != NULL);
  ECHO_ASSERT(header != NULL);

  /* Check if header connects */
  if (!header_connects(engine, header)) {
    return false;
  }

  /* Get parent index */
  const block_index_t *parent =
      consensus_lookup_block_index(engine, &header->prev_hash);
  if (parent == NULL) {
    /* Check if this is genesis */
    bool is_genesis = true;
    for (int i = 0; i < 32; i++) {
      if (header->prev_hash.bytes[i] != 0) {
        is_genesis = false;
        break;
      }
    }
    if (!is_genesis) {
      return false;
    }
  }

  /* Current tip */
  block_index_t *tip_index = chainstate_get_tip_index(engine->chainstate);
  if (tip_index == NULL) {
    return false; /* No current tip, not a reorg */
  }

  /* Check if parent is the current tip */
  if (parent != NULL &&
      memcmp(parent->hash.bytes, tip_index->hash.bytes, 32) == 0) {
    return false; /* Extends current tip, not a reorg */
  }

  /* Would need to check if new chain has more work */
  /* For simplicity, we'd need to compute the work of the new block */
  /* and compare with current tip */
  return parent != NULL && parent->height >= tip_index->height;
}

/**
 * Callback adapter for chain_reorganize.
 */
typedef struct {
  consensus_get_block_fn get_block;
  void *user_data;
} reorg_callback_ctx_t;

static echo_result_t reorg_get_block_txs(const hash256_t *block_hash,
                                         const tx_t **txs_out,
                                         size_t *tx_count_out,
                                         void *user_data) {
  reorg_callback_ctx_t *ctx = (reorg_callback_ctx_t *)user_data;

  /* Allocate a block to receive data */
  static block_t temp_block; /* Note: Not thread-safe, simplified for now */
  block_init(&temp_block);

  echo_result_t result =
      ctx->get_block(block_hash, &temp_block, ctx->user_data);
  if (result != ECHO_OK) {
    return result;
  }

  *txs_out = temp_block.txs;
  *tx_count_out = temp_block.tx_count;

  return ECHO_OK;
}

echo_result_t consensus_reorganize(consensus_engine_t *engine,
                                   const hash256_t *new_tip_hash,
                                   consensus_get_block_fn get_block,
                                   void *user_data,
                                   consensus_result_t *result) {
  ECHO_ASSERT(engine != NULL);
  ECHO_ASSERT(new_tip_hash != NULL);
  ECHO_ASSERT(get_block != NULL);

  if (result != NULL) {
    consensus_result_init(result);
  }

  /* Find the new tip in our index */
  block_index_t *new_tip_index = block_index_map_lookup(
      chainstate_get_block_index_map(engine->chainstate), new_tip_hash);

  if (new_tip_index == NULL) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_INVALID_PREV;
    }
    return ECHO_ERR_NOT_FOUND;
  }

  /* Get current tip */
  block_index_t *current_tip = chainstate_get_tip_index(engine->chainstate);
  if (current_tip == NULL) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_INTERNAL;
    }
    return ECHO_ERR_NOT_FOUND;
  }

  /* Create reorg plan */
  chain_reorg_t *reorg = chain_reorg_create(current_tip, new_tip_index);
  if (reorg == NULL) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_NOMEM;
    }
    return ECHO_ERR_NOMEM;
  }

  /* Set up callback context */
  reorg_callback_ctx_t callback_ctx = {.get_block = get_block,
                                       .user_data = user_data};

  /* Execute reorganization */
  echo_result_t reorg_result = chain_reorganize(
      engine->chainstate, reorg, reorg_get_block_txs, &callback_ctx);

  chain_reorg_destroy(reorg);

  if (reorg_result != ECHO_OK) {
    if (result != NULL) {
      result->error = CONSENSUS_ERR_REORG_FAILED;
    }
    return reorg_result;
  }

  return ECHO_OK;
}

/*
 * ============================================================================
 * STATISTICS
 * ============================================================================
 */

void consensus_get_stats(const consensus_engine_t *engine,
                         consensus_stats_t *stats) {
  ECHO_ASSERT(engine != NULL);
  ECHO_ASSERT(stats != NULL);

  memset(stats, 0, sizeof(consensus_stats_t));

  /* Always report block_index_count - we may have headers before blocks */
  stats->block_index_count = consensus_block_index_count(engine);

  if (!engine->initialized) {
    return;
  }

  chain_tip_t tip;
  chainstate_get_tip(engine->chainstate, &tip);

  stats->height = tip.height;
  stats->total_work = tip.chainwork;
  stats->utxo_count = consensus_utxo_count(engine);

  /* Total coins is calculated from subsidy schedule */
  /* For simplicity, we don't track this precisely */
  stats->total_coins = 0;
}

chainstate_t *consensus_get_chainstate(consensus_engine_t *engine) {
  ECHO_ASSERT(engine != NULL);
  return engine->chainstate;
}

const utxo_set_t *consensus_get_utxo_set(const consensus_engine_t *engine) {
  ECHO_ASSERT(engine != NULL);
  return chainstate_get_utxo_set(engine->chainstate);
}
