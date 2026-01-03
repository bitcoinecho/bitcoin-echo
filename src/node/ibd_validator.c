/*
 * Bitcoin Echo — IBD Chunk Validator Implementation
 *
 * Validates consecutive chunks of blocks during Initial Block Download.
 *
 * See ibd_validator.h for design overview.
 *
 * Build once. Build right. Stop.
 */

#include "ibd_validator.h"
#include "block.h"
#include "block_validate.h"
#include "chainstate.h"
#include "db.h"
#include "log.h"
#include "node.h"
#include "platform.h"
#include "tx.h"
#include "utxo.h"
#include "utxo_db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * IBD UTXO Batch Implementation
 * ============================================================================
 */

ibd_utxo_batch_t *ibd_utxo_batch_create(uint32_t start_height,
                                        uint32_t end_height) {
  ibd_utxo_batch_t *batch = calloc(1, sizeof(ibd_utxo_batch_t));
  if (batch == NULL) {
    return NULL;
  }

  batch->created_utxos = utxo_set_create(IBD_BATCH_INITIAL_CAPACITY);
  if (batch->created_utxos == NULL) {
    free(batch);
    return NULL;
  }

  /* Pre-allocate spent outpoints array */
  batch->spent_capacity = IBD_BATCH_INITIAL_CAPACITY;
  batch->spent_outpoints = calloc(batch->spent_capacity, sizeof(outpoint_t));
  if (batch->spent_outpoints == NULL) {
    utxo_set_destroy(batch->created_utxos);
    free(batch);
    return NULL;
  }

  batch->chunk_start_height = start_height;
  batch->chunk_end_height = end_height;
  batch->spent_count = 0;
  batch->created_then_spent_count = 0;
  batch->total_txs_processed = 0;
  batch->total_inputs_processed = 0;
  batch->total_outputs_processed = 0;

  return batch;
}

void ibd_utxo_batch_destroy(ibd_utxo_batch_t *batch) {
  if (batch == NULL) {
    return;
  }

  if (batch->created_utxos != NULL) {
    utxo_set_destroy(batch->created_utxos);
  }

  free(batch->spent_outpoints);
  free(batch);
}

echo_result_t ibd_utxo_batch_add_created(ibd_utxo_batch_t *batch,
                                         const utxo_entry_t *entry) {
  if (batch == NULL || entry == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  echo_result_t result = utxo_set_insert(batch->created_utxos, entry);
  if (result == ECHO_OK) {
    batch->total_outputs_processed++;
  }

  return result;
}

echo_result_t ibd_utxo_batch_add_spent(ibd_utxo_batch_t *batch,
                                       const outpoint_t *outpoint) {
  if (batch == NULL || outpoint == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  batch->total_inputs_processed++;

  /* Check if this UTXO was created within this chunk */
  const utxo_entry_t *created = utxo_set_lookup(batch->created_utxos, outpoint);
  if (created != NULL) {
    /* Created and spent within same chunk - remove from created set.
     * This UTXO will never touch the database. */
    utxo_set_remove(batch->created_utxos, outpoint);
    batch->created_then_spent_count++;
    return ECHO_OK;
  }

  /* UTXO existed before this chunk - add to spent list */
  if (batch->spent_count >= batch->spent_capacity) {
    /* Grow array */
    size_t new_capacity = batch->spent_capacity * 2;
    outpoint_t *new_array =
        realloc(batch->spent_outpoints, new_capacity * sizeof(outpoint_t));
    if (new_array == NULL) {
      return ECHO_ERR_MEMORY;
    }
    batch->spent_outpoints = new_array;
    batch->spent_capacity = new_capacity;
  }

  memcpy(&batch->spent_outpoints[batch->spent_count], outpoint,
         sizeof(outpoint_t));
  batch->spent_count++;

  return ECHO_OK;
}

const utxo_entry_t *ibd_utxo_batch_lookup(const ibd_utxo_batch_t *batch,
                                          const outpoint_t *outpoint) {
  if (batch == NULL || outpoint == NULL) {
    return NULL;
  }

  return utxo_set_lookup(batch->created_utxos, outpoint);
}

void ibd_utxo_batch_get_stats(const ibd_utxo_batch_t *batch,
                              size_t *created_count, size_t *spent_count,
                              size_t *cancelled_count) {
  if (batch == NULL) {
    if (created_count != NULL) *created_count = 0;
    if (spent_count != NULL) *spent_count = 0;
    if (cancelled_count != NULL) *cancelled_count = 0;
    return;
  }

  if (created_count != NULL) {
    *created_count = utxo_set_size(batch->created_utxos);
  }
  if (spent_count != NULL) {
    *spent_count = batch->spent_count;
  }
  if (cancelled_count != NULL) {
    *cancelled_count = batch->created_then_spent_count;
  }
}

/* ============================================================================
 * IBD Validator Implementation
 * ============================================================================
 */

ibd_validator_t *ibd_validator_create(node_t *node, chainstate_t *chainstate,
                                      utxo_db_t *utxo_db, uint32_t start_height,
                                      uint32_t end_height, bool skip_scripts) {
  if (node == NULL || chainstate == NULL) {
    return NULL;
  }

  /* Validate range */
  if (start_height > end_height) {
    log_error(LOG_COMP_SYNC, "ibd_validator: invalid range %u > %u",
              start_height, end_height);
    return NULL;
  }

  /* Calculate chunk size for logging */
  uint32_t chunk_size = end_height - start_height + 1;

  ibd_validator_t *validator = calloc(1, sizeof(ibd_validator_t));
  if (validator == NULL) {
    return NULL;
  }

  validator->batch = ibd_utxo_batch_create(start_height, end_height);
  if (validator->batch == NULL) {
    free(validator);
    return NULL;
  }

  validator->node = node;
  validator->chainstate = chainstate;
  validator->utxo_db = utxo_db;
  validator->start_height = start_height;
  validator->end_height = end_height;
  validator->current_height = start_height;
  validator->skip_script_validation = skip_scripts;
  validator->last_error = IBD_VALID_OK;
  validator->error_height = 0;
  validator->error_message[0] = '\0';

  log_info(LOG_COMP_SYNC,
           "ibd_validator: created for blocks %u-%u (%u blocks, scripts=%s)",
           start_height, end_height, chunk_size,
           skip_scripts ? "skip" : "verify");

  return validator;
}

void ibd_validator_destroy(ibd_validator_t *validator) {
  if (validator == NULL) {
    return;
  }

  ibd_utxo_batch_destroy(validator->batch);
  free(validator);
}

/*
 * Look up a UTXO for spending - check batch first, then DB.
 *
 * Returns:
 *   Pointer to UTXO entry if found
 *   NULL if not found
 *
 * Note: If found in DB, the caller receives an allocated entry that must be
 * freed. Set *needs_free = true in that case.
 */
static const utxo_entry_t *
lookup_utxo_for_spend(ibd_validator_t *validator, const outpoint_t *outpoint,
                      utxo_entry_t **db_entry_out, bool *needs_free) {
  *needs_free = false;
  *db_entry_out = NULL;

  /* First check if UTXO was created within this chunk */
  const utxo_entry_t *batch_entry =
      ibd_utxo_batch_lookup(validator->batch, outpoint);
  if (batch_entry != NULL) {
    return batch_entry;
  }

  /* Check database for pre-existing UTXO */
  if (validator->utxo_db != NULL) {
    utxo_entry_t *db_entry = NULL;
    if (utxo_db_lookup(validator->utxo_db, outpoint, &db_entry) == ECHO_OK) {
      *db_entry_out = db_entry;
      *needs_free = true;
      return db_entry;
    }
  }

  /* Also check in-memory chainstate UTXO set (for recently validated blocks) */
  const utxo_set_t *utxo_set = chainstate_get_utxo_set(validator->chainstate);
  if (utxo_set != NULL) {
    const utxo_entry_t *cs_entry = utxo_set_lookup(utxo_set, outpoint);
    if (cs_entry != NULL) {
      return cs_entry;
    }
  }

  return NULL;
}

/*
 * Validate a single transaction's inputs and outputs.
 *
 * Updates the UTXO batch with spent/created entries.
 */
static ibd_valid_result_t
validate_tx_utxos(ibd_validator_t *validator, const tx_t *tx,
                  const hash256_t *txid, uint32_t height, bool is_coinbase,
                  satoshi_t *input_sum_out, satoshi_t *output_sum_out) {
  satoshi_t input_sum = 0;
  satoshi_t output_sum = 0;

  /* Process inputs (skip for coinbase) */
  if (!is_coinbase) {
    for (size_t i = 0; i < tx->input_count; i++) {
      const tx_input_t *input = &tx->inputs[i];
      const outpoint_t *outpoint = &input->prevout;

      /* Look up the UTXO being spent */
      utxo_entry_t *db_entry = NULL;
      bool needs_free = false;
      const utxo_entry_t *utxo =
          lookup_utxo_for_spend(validator, outpoint, &db_entry, &needs_free);

      if (utxo == NULL) {
        snprintf(validator->error_message, sizeof(validator->error_message),
                 "Missing UTXO for input %zu of tx at height %u", i, height);
        return IBD_VALID_ERR_UTXO_MISSING;
      }

      /* Check coinbase maturity */
      if (utxo->is_coinbase) {
        if (height < utxo->height + COINBASE_MATURITY) {
          if (needs_free) {
            utxo_entry_destroy(db_entry);
          }
          snprintf(validator->error_message, sizeof(validator->error_message),
                   "Immature coinbase spend at height %u (UTXO height %u)",
                   height, utxo->height);
          return IBD_VALID_ERR_UTXO_MISSING;
        }
      }

      input_sum += utxo->value;

      /* Free DB entry if we allocated it */
      if (needs_free) {
        utxo_entry_destroy(db_entry);
      }

      /* Mark UTXO as spent in batch */
      echo_result_t result = ibd_utxo_batch_add_spent(validator->batch, outpoint);
      if (result != ECHO_OK) {
        snprintf(validator->error_message, sizeof(validator->error_message),
                 "Failed to mark UTXO spent at height %u", height);
        return IBD_VALID_ERR_MEMORY;
      }
    }
  }

  /* Process outputs - create new UTXOs */
  for (size_t i = 0; i < tx->output_count; i++) {
    const tx_output_t *output = &tx->outputs[i];

    /* Skip OP_RETURN outputs (unspendable) */
    if (output->script_pubkey_len > 0 && output->script_pubkey[0] == 0x6a) {
      continue;
    }

    output_sum += output->value;

    /* Create UTXO entry */
    outpoint_t new_outpoint;
    memcpy(new_outpoint.txid.bytes, txid->bytes, 32);
    new_outpoint.vout = (uint32_t)i;

    utxo_entry_t *entry = utxo_entry_create(
        &new_outpoint, output->value, output->script_pubkey,
        output->script_pubkey_len, height, is_coinbase);
    if (entry == NULL) {
      snprintf(validator->error_message, sizeof(validator->error_message),
               "Failed to create UTXO entry at height %u", height);
      return IBD_VALID_ERR_MEMORY;
    }

    /* Add to batch */
    echo_result_t result = ibd_utxo_batch_add_created(validator->batch, entry);
    utxo_entry_destroy(entry); /* batch clones it */
    if (result != ECHO_OK && result != ECHO_ERR_EXISTS) {
      snprintf(validator->error_message, sizeof(validator->error_message),
               "Failed to add UTXO to batch at height %u", height);
      return IBD_VALID_ERR_MEMORY;
    }
  }

  *input_sum_out = input_sum;
  *output_sum_out = output_sum;
  return IBD_VALID_OK;
}

ibd_valid_result_t ibd_validator_validate_next(ibd_validator_t *validator) {
  if (validator == NULL) {
    return IBD_VALID_ERR_INTERNAL;
  }

  if (validator->current_height > validator->end_height) {
    return IBD_VALID_OK; /* Already complete */
  }

  uint32_t height = validator->current_height;

  /* Load block from disk */
  block_t block;
  hash256_t block_hash;
  echo_result_t load_result =
      node_load_block_at_height(validator->node, height, &block, &block_hash);

  if (load_result != ECHO_OK) {
    validator->last_error = IBD_VALID_ERR_LOAD;
    validator->error_height = height;
    snprintf(validator->error_message, sizeof(validator->error_message),
             "Failed to load block at height %u: %d", height, load_result);
    return IBD_VALID_ERR_LOAD;
  }

  /* Validate proof-of-work */
  block_validation_error_t pow_error = BLOCK_VALID;
  if (!block_validate_pow_with_hash(&block.header, &block_hash, &pow_error)) {
    block_free(&block);
    validator->last_error = IBD_VALID_ERR_POW;
    validator->error_height = height;
    snprintf(validator->error_message, sizeof(validator->error_message),
             "PoW validation failed at height %u: %s", height,
             block_validation_error_str(pow_error));
    return IBD_VALID_ERR_POW;
  }

  /* Validate merkle root */
  block_validation_error_t merkle_error = BLOCK_VALID;
  if (!block_validate_merkle_root(&block, &merkle_error)) {
    block_free(&block);
    validator->last_error = IBD_VALID_ERR_MERKLE;
    validator->error_height = height;
    snprintf(validator->error_message, sizeof(validator->error_message),
             "Merkle root mismatch at height %u", height);
    return IBD_VALID_ERR_MERKLE;
  }

  /* Validate block structure (coinbase first, no duplicates, etc.) */
  block_validation_error_t struct_error = BLOCK_VALID;
  if (!block_validate_tx_structure(&block, &struct_error)) {
    block_free(&block);
    validator->last_error = IBD_VALID_ERR_STRUCTURE;
    validator->error_height = height;
    snprintf(validator->error_message, sizeof(validator->error_message),
             "Block structure invalid at height %u: %s", height,
             block_validation_error_str(struct_error));
    return IBD_VALID_ERR_STRUCTURE;
  }

  /* Compute TXIDs for all transactions */
  hash256_t *txids = malloc(block.tx_count * sizeof(hash256_t));
  if (txids == NULL) {
    block_free(&block);
    validator->last_error = IBD_VALID_ERR_MEMORY;
    validator->error_height = height;
    snprintf(validator->error_message, sizeof(validator->error_message),
             "Failed to allocate TXIDs at height %u", height);
    return IBD_VALID_ERR_MEMORY;
  }

  for (size_t i = 0; i < block.tx_count; i++) {
    tx_compute_txid(&block.txs[i], &txids[i]);
  }

  /* Validate transactions and update UTXO batch */
  satoshi_t total_fees = 0;
  ibd_valid_result_t result = IBD_VALID_OK;

  for (size_t i = 0; i < block.tx_count; i++) {
    const tx_t *tx = &block.txs[i];
    bool is_coinbase = (i == 0);

    satoshi_t input_sum = 0;
    satoshi_t output_sum = 0;

    result = validate_tx_utxos(validator, tx, &txids[i], height, is_coinbase,
                               &input_sum, &output_sum);
    if (result != IBD_VALID_OK) {
      validator->error_height = height;
      break;
    }

    /* Value accounting for non-coinbase transactions */
    if (!is_coinbase) {
      if (output_sum > input_sum) {
        result = IBD_VALID_ERR_VALUE;
        validator->error_height = height;
        snprintf(validator->error_message, sizeof(validator->error_message),
                 "Output value exceeds input at height %u tx %zu", height, i);
        break;
      }
      total_fees += (input_sum - output_sum);
    }

    validator->batch->total_txs_processed++;
  }

  /* Validate coinbase value (subsidy + fees) */
  if (result == IBD_VALID_OK && block.tx_count > 0) {
    satoshi_t subsidy = coinbase_subsidy(height);
    satoshi_t max_coinbase = subsidy + total_fees;

    satoshi_t coinbase_output = 0;
    for (size_t i = 0; i < block.txs[0].output_count; i++) {
      coinbase_output += block.txs[0].outputs[i].value;
    }

    if (coinbase_output > max_coinbase) {
      result = IBD_VALID_ERR_COINBASE;
      validator->error_height = height;
      snprintf(validator->error_message, sizeof(validator->error_message),
               "Coinbase output %llu exceeds max %llu at height %u",
               (unsigned long long)coinbase_output,
               (unsigned long long)max_coinbase, height);
    }
  }

  /* Script validation (if not skipping) */
  if (result == IBD_VALID_OK && !validator->skip_script_validation) {
    /* TODO: Implement script validation using existing infrastructure.
     * For now, we rely on assumevalid skipping scripts for most blocks.
     * This will be fully implemented when we process post-assumevalid blocks.
     */
  }

  free(txids);
  block_free(&block);

  if (result == IBD_VALID_OK) {
    validator->current_height++;

    /* Progress logging */
    uint32_t blocks_done = validator->current_height - validator->start_height;
    if (blocks_done % IBD_PROGRESS_LOG_INTERVAL == 0 ||
        validator->current_height > validator->end_height) {
      log_debug(LOG_COMP_SYNC,
                "ibd_validator: validated %u/%u blocks (current: %u)",
                blocks_done, validator->end_height - validator->start_height + 1,
                validator->current_height - 1);
    }
  } else {
    validator->last_error = result;
  }

  return result;
}

ibd_valid_result_t ibd_validator_validate_chunk(ibd_validator_t *validator) {
  if (validator == NULL) {
    return IBD_VALID_ERR_INTERNAL;
  }

  uint64_t start_time = plat_time_ms();
  uint32_t start_height = validator->current_height;

  while (validator->current_height <= validator->end_height) {
    ibd_valid_result_t result = ibd_validator_validate_next(validator);
    if (result != IBD_VALID_OK) {
      return result;
    }
  }

  uint64_t elapsed = plat_time_ms() - start_time;
  uint32_t blocks_validated = validator->current_height - start_height;

  size_t created, spent, cancelled;
  ibd_utxo_batch_get_stats(validator->batch, &created, &spent, &cancelled);

  log_info(LOG_COMP_SYNC,
           "ibd_validator: chunk %u-%u complete in %llu ms "
           "(%.1f blk/s, %zu created, %zu spent, %zu cancelled)",
           validator->start_height, validator->end_height,
           (unsigned long long)elapsed,
           elapsed > 0 ? (blocks_validated * 1000.0 / (double)elapsed) : 0.0,
           created, spent, cancelled);

  return IBD_VALID_OK;
}

/* Callback context for flush iteration */
typedef struct {
  utxo_db_t *utxo_db;
  echo_result_t result;
  size_t count;
} flush_ctx_t;

/* Callback for inserting UTXOs during flush */
static bool flush_insert_callback(const utxo_entry_t *entry, void *user_data) {
  flush_ctx_t *ctx = (flush_ctx_t *)user_data;

  echo_result_t result = utxo_db_insert(ctx->utxo_db, entry);
  if (result != ECHO_OK && result != ECHO_ERR_EXISTS) {
    ctx->result = result;
    return false; /* Stop iteration */
  }

  ctx->count++;
  return true; /* Continue */
}

echo_result_t ibd_validator_flush(ibd_validator_t *validator) {
  if (validator == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (validator->current_height <= validator->end_height) {
    log_warn(LOG_COMP_SYNC,
             "ibd_validator: flush called before validation complete "
             "(at %u, end %u)",
             validator->current_height, validator->end_height);
  }

  if (validator->utxo_db == NULL) {
    log_warn(LOG_COMP_SYNC, "ibd_validator: no UTXO DB, skipping flush");
    return ECHO_OK;
  }

  size_t created, spent, cancelled;
  ibd_utxo_batch_get_stats(validator->batch, &created, &spent, &cancelled);

  log_info(LOG_COMP_SYNC,
           "ibd_validator: flushing chunk %u-%u "
           "(%zu inserts, %zu deletes, %zu cancelled)",
           validator->start_height, validator->end_height, created, spent,
           cancelled);

  uint64_t start_time = plat_time_ms();

  /* Begin atomic transaction using underlying db */
  echo_result_t result = db_begin(&validator->utxo_db->db);
  if (result != ECHO_OK) {
    log_error(LOG_COMP_SYNC, "ibd_validator: failed to begin transaction: %d",
              result);
    return result;
  }

  /* Delete spent UTXOs */
  for (size_t i = 0; i < validator->batch->spent_count; i++) {
    result =
        utxo_db_delete(validator->utxo_db, &validator->batch->spent_outpoints[i]);
    if (result != ECHO_OK && result != ECHO_ERR_NOT_FOUND) {
      log_error(LOG_COMP_SYNC,
                "ibd_validator: failed to delete UTXO %zu: %d", i, result);
      db_rollback(&validator->utxo_db->db);
      return result;
    }
  }

  /* Insert created UTXOs using foreach */
  flush_ctx_t flush_ctx = {
      .utxo_db = validator->utxo_db,
      .result = ECHO_OK,
      .count = 0
  };

  utxo_set_foreach(validator->batch->created_utxos, flush_insert_callback,
                   &flush_ctx);

  if (flush_ctx.result != ECHO_OK) {
    log_error(LOG_COMP_SYNC, "ibd_validator: failed to insert UTXO: %d",
              flush_ctx.result);
    db_rollback(&validator->utxo_db->db);
    return flush_ctx.result;
  }

  /* Commit transaction */
  result = db_commit(&validator->utxo_db->db);
  if (result != ECHO_OK) {
    log_error(LOG_COMP_SYNC, "ibd_validator: failed to commit: %d", result);
    return result;
  }

  uint64_t elapsed = plat_time_ms() - start_time;
  log_info(LOG_COMP_SYNC,
           "ibd_validator: flush complete in %llu ms (%zu inserts)",
           (unsigned long long)elapsed, flush_ctx.count);

  return ECHO_OK;
}

void ibd_validator_get_progress(const ibd_validator_t *validator,
                                uint32_t *current_height, uint32_t *total_blocks,
                                uint32_t *blocks_done) {
  if (validator == NULL) {
    if (current_height != NULL) *current_height = 0;
    if (total_blocks != NULL) *total_blocks = 0;
    if (blocks_done != NULL) *blocks_done = 0;
    return;
  }

  if (current_height != NULL) {
    *current_height = validator->current_height;
  }
  if (total_blocks != NULL) {
    *total_blocks = validator->end_height - validator->start_height + 1;
  }
  if (blocks_done != NULL) {
    *blocks_done = validator->current_height - validator->start_height;
  }
}

bool ibd_validator_is_complete(const ibd_validator_t *validator) {
  if (validator == NULL) {
    return false;
  }
  return validator->current_height > validator->end_height;
}

ibd_valid_result_t ibd_validator_get_error(const ibd_validator_t *validator,
                                           uint32_t *height,
                                           const char **message) {
  if (validator == NULL) {
    if (height != NULL) *height = 0;
    if (message != NULL) *message = NULL;
    return IBD_VALID_ERR_INTERNAL;
  }

  if (height != NULL) {
    *height = validator->error_height;
  }
  if (message != NULL) {
    *message = validator->error_message[0] != '\0' ? validator->error_message
                                                   : NULL;
  }

  return validator->last_error;
}

const char *ibd_valid_result_string(ibd_valid_result_t result) {
  switch (result) {
  case IBD_VALID_OK:
    return "OK";
  case IBD_VALID_ERR_LOAD:
    return "LOAD_ERROR";
  case IBD_VALID_ERR_POW:
    return "POW_FAILED";
  case IBD_VALID_ERR_MERKLE:
    return "MERKLE_MISMATCH";
  case IBD_VALID_ERR_STRUCTURE:
    return "STRUCTURE_INVALID";
  case IBD_VALID_ERR_UTXO_MISSING:
    return "UTXO_MISSING";
  case IBD_VALID_ERR_UTXO_DOUBLE:
    return "DOUBLE_SPEND";
  case IBD_VALID_ERR_VALUE:
    return "VALUE_ERROR";
  case IBD_VALID_ERR_SCRIPT:
    return "SCRIPT_FAILED";
  case IBD_VALID_ERR_COINBASE:
    return "COINBASE_ERROR";
  case IBD_VALID_ERR_MEMORY:
    return "MEMORY_ERROR";
  case IBD_VALID_ERR_INTERNAL:
    return "INTERNAL_ERROR";
  default:
    return "UNKNOWN_ERROR";
  }
}
