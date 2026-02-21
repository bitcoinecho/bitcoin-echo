/**
 * Bitcoin Echo — Transaction Memory Pool Implementation
 *
 * The mempool holds unconfirmed transactions awaiting inclusion in a block.
 *
 * Implementation notes:
 * - Hash table for O(1) txid lookup
 * - Separate hash table for wtxid lookup
 * - Hash table for spent outpoints (conflict detection)
 * - Sorted list by fee rate for mining selection
 * - Ancestor/descendant tracking for package limits
 *
 * Build once. Build right. Stop.
 */

#include "mempool.h"
#include "block.h"
#include "echo_types.h"
#include "tx.h"
#include "tx_validate.h"
#include "utxo.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * ============================================================================
 * INTERNAL CONSTANTS
 * ============================================================================
 */

/* Hash table size (prime number for better distribution) */
#define TXID_TABLE_SIZE 65537
#define WTXID_TABLE_SIZE 65537
#define SPENT_TABLE_SIZE 131071

/*
 * ============================================================================
 * INTERNAL STRUCTURES
 * ============================================================================
 */

/**
 * Spent outpoint entry for conflict detection.
 */
typedef struct spent_entry {
  outpoint_t outpoint;
  mempool_entry_t *spender; /* Transaction that spends this outpoint */
  struct spent_entry *next; /* Hash chain */
} spent_entry_t;

/**
 * Fee rate sorted node for mining selection.
 */
typedef struct fee_node {
  mempool_entry_t *entry;
  struct fee_node *prev;
  struct fee_node *next;
} fee_node_t;

/**
 * Mempool structure.
 */
struct mempool {
  /* Configuration */
  mempool_config_t config;

  /* Callbacks */
  mempool_callbacks_t callbacks;
  bool has_callbacks;

  /* Transaction storage: hash table by txid */
  mempool_entry_t *txid_table[TXID_TABLE_SIZE];

  /* Secondary index: hash table by wtxid */
  mempool_entry_t *wtxid_table[WTXID_TABLE_SIZE];

  /* Spent outpoint tracking */
  spent_entry_t *spent_table[SPENT_TABLE_SIZE];

  /* Fee-sorted list for mining */
  fee_node_t *fee_list_head; /* Highest fee rate */
  fee_node_t *fee_list_tail; /* Lowest fee rate */

  /* Statistics */
  size_t tx_count;
  size_t total_bytes;
  size_t total_vsize;
  satoshi_t total_fees;
};

/**
 * Iterator structure.
 */
struct mempool_iter {
  const mempool_t *mp;
  fee_node_t *current;
};

/*
 * ============================================================================
 * HASH FUNCTIONS
 * ============================================================================
 */

/**
 * Hash a 256-bit hash to table index.
 */
static size_t hash_to_index(const hash256_t *hash, size_t table_size) {
  /* Use first 8 bytes as a 64-bit number */
  uint64_t h = 0;
  for (int i = 0; i < 8; i++) {
    h = (h << 8) | hash->bytes[i];
  }
  return (size_t)(h % table_size);
}

/**
 * Hash an outpoint to table index.
 */
static size_t outpoint_to_index(const outpoint_t *op, size_t table_size) {
  /* Combine txid hash with vout */
  uint64_t h = 0;
  for (int i = 0; i < 8; i++) {
    h = (h << 8) | op->txid.bytes[i];
  }
  h ^= (uint64_t)op->vout;
  return (size_t)(h % table_size);
}

/*
 * ============================================================================
 * ENTRY MANAGEMENT
 * ============================================================================
 */

/**
 * Create a mempool entry from a transaction.
 */
static mempool_entry_t *entry_create(const tx_t *tx, satoshi_t fee,
                                     uint64_t time_added,
                                     uint32_t height_added) {
  mempool_entry_t *entry = calloc(1, sizeof(mempool_entry_t));
  if (entry == NULL) {
    return NULL;
  }

  /* Copy transaction */
  tx_init(&entry->tx);

  /* Deep copy: allocate and copy inputs */
  if (tx->input_count > 0) {
    entry->tx.inputs = calloc(tx->input_count, sizeof(tx_input_t));
    if (entry->tx.inputs == NULL) {
      free(entry);
      return NULL;
    }

    for (size_t i = 0; i < tx->input_count; i++) {
      entry->tx.inputs[i].prevout = tx->inputs[i].prevout;
      entry->tx.inputs[i].sequence = tx->inputs[i].sequence;

      /* Copy scriptSig */
      if (tx->inputs[i].script_sig_len > 0) {
        entry->tx.inputs[i].script_sig = malloc(tx->inputs[i].script_sig_len);
        if (entry->tx.inputs[i].script_sig == NULL) {
          /* Cleanup on failure */
          for (size_t j = 0; j < i; j++) {
            free(entry->tx.inputs[j].script_sig);
          }
          free(entry->tx.inputs);
          free(entry);
          return NULL;
        }
        memcpy(entry->tx.inputs[i].script_sig, tx->inputs[i].script_sig,
               tx->inputs[i].script_sig_len);
        entry->tx.inputs[i].script_sig_len = tx->inputs[i].script_sig_len;
      }

      /* Copy witness */
      if (tx->inputs[i].witness.count > 0) {
        entry->tx.inputs[i].witness.items =
            calloc(tx->inputs[i].witness.count, sizeof(witness_item_t));
        if (entry->tx.inputs[i].witness.items == NULL) {
          /* Cleanup on failure - complex, so just fail */
          tx_free(&entry->tx);
          free(entry);
          return NULL;
        }
        entry->tx.inputs[i].witness.count = tx->inputs[i].witness.count;

        for (size_t w = 0; w < tx->inputs[i].witness.count; w++) {
          size_t wlen = tx->inputs[i].witness.items[w].len;
          if (wlen > 0) {
            entry->tx.inputs[i].witness.items[w].data = malloc(wlen);
            if (entry->tx.inputs[i].witness.items[w].data == NULL) {
              tx_free(&entry->tx);
              free(entry);
              return NULL;
            }
            memcpy(entry->tx.inputs[i].witness.items[w].data,
                   tx->inputs[i].witness.items[w].data, wlen);
            entry->tx.inputs[i].witness.items[w].len = wlen;
          }
        }
      }
    }
  }

  /* Copy outputs */
  if (tx->output_count > 0) {
    entry->tx.outputs = calloc(tx->output_count, sizeof(tx_output_t));
    if (entry->tx.outputs == NULL) {
      tx_free(&entry->tx);
      free(entry);
      return NULL;
    }

    for (size_t i = 0; i < tx->output_count; i++) {
      entry->tx.outputs[i].value = tx->outputs[i].value;

      if (tx->outputs[i].script_pubkey_len > 0) {
        entry->tx.outputs[i].script_pubkey =
            malloc(tx->outputs[i].script_pubkey_len);
        if (entry->tx.outputs[i].script_pubkey == NULL) {
          tx_free(&entry->tx);
          free(entry);
          return NULL;
        }
        memcpy(entry->tx.outputs[i].script_pubkey, tx->outputs[i].script_pubkey,
               tx->outputs[i].script_pubkey_len);
        entry->tx.outputs[i].script_pubkey_len =
            tx->outputs[i].script_pubkey_len;
      }
    }
  }

  /* Copy scalar fields */
  entry->tx.version = tx->version;
  entry->tx.input_count = tx->input_count;
  entry->tx.output_count = tx->output_count;
  entry->tx.locktime = tx->locktime;
  entry->tx.has_witness = tx->has_witness;

  /* Compute and cache IDs */
  tx_compute_txid(&entry->tx, &entry->txid);
  tx_compute_wtxid(&entry->tx, &entry->wtxid);

  /* Set metadata */
  entry->fee = fee;
  entry->vsize = tx_vsize(&entry->tx);
  entry->fee_rate = mempool_calc_fee_rate(fee, entry->vsize);
  entry->time_added = time_added;
  entry->height_added = height_added;

  /* Initialize ancestor/descendant counts */
  entry->ancestor_count = 1;
  entry->descendant_count = 1;
  entry->ancestor_fees = fee;
  entry->ancestor_size = entry->vsize;
  entry->descendant_fees = fee;
  entry->descendant_size = entry->vsize;

  /* Check RBF signaling (any input with sequence < MAX-1) */
  entry->signals_rbf = false;
  for (size_t i = 0; i < tx->input_count; i++) {
    if (tx->inputs[i].sequence < TX_SEQUENCE_DISABLE_RBF) {
      entry->signals_rbf = true;
      break;
    }
  }

  return entry;
}

/**
 * Destroy a mempool entry.
 */
static void entry_destroy(mempool_entry_t *entry) {
  if (entry == NULL) {
    return;
  }
  tx_free(&entry->tx);
  free(entry);
}

/*
 * ============================================================================
 * SPENT OUTPOINT TRACKING
 * ============================================================================
 */

/**
 * Add spent outpoint to tracking.
 */
static echo_result_t spent_add(mempool_t *mp, const outpoint_t *op,
                               mempool_entry_t *spender) {
  size_t idx = outpoint_to_index(op, SPENT_TABLE_SIZE);

  spent_entry_t *entry = malloc(sizeof(spent_entry_t));
  if (entry == NULL) {
    return ECHO_ERR_MEMORY;
  }

  entry->outpoint = *op;
  entry->spender = spender;
  entry->next = mp->spent_table[idx];
  mp->spent_table[idx] = entry;

  return ECHO_OK;
}

/**
 * Remove spent outpoint from tracking.
 */
static void spent_remove(mempool_t *mp, const outpoint_t *op) {
  size_t idx = outpoint_to_index(op, SPENT_TABLE_SIZE);

  spent_entry_t **prev = &mp->spent_table[idx];
  spent_entry_t *curr = mp->spent_table[idx];

  while (curr != NULL) {
    if (outpoint_equal(&curr->outpoint, op)) {
      *prev = curr->next;
      free(curr);
      return;
    }
    prev = &curr->next;
    curr = curr->next;
  }
}

/**
 * Look up spent outpoint.
 */
static mempool_entry_t *spent_lookup(const mempool_t *mp,
                                     const outpoint_t *op) {
  size_t idx = outpoint_to_index(op, SPENT_TABLE_SIZE);

  spent_entry_t *curr = mp->spent_table[idx];
  while (curr != NULL) {
    if (outpoint_equal(&curr->outpoint, op)) {
      return curr->spender;
    }
    curr = curr->next;
  }

  return NULL;
}

/*
 * ============================================================================
 * FEE-SORTED LIST
 * ============================================================================
 */

/**
 * Insert entry into fee-sorted list.
 */
static echo_result_t fee_list_insert(mempool_t *mp, mempool_entry_t *entry) {
  fee_node_t *node = malloc(sizeof(fee_node_t));
  if (node == NULL) {
    return ECHO_ERR_MEMORY;
  }

  node->entry = entry;

  /* Find insertion point (descending order by fee rate) */
  fee_node_t *curr = mp->fee_list_head;
  while (curr != NULL && curr->entry->fee_rate > entry->fee_rate) {
    curr = curr->next;
  }

  if (curr == NULL) {
    /* Insert at tail */
    node->next = NULL;
    node->prev = mp->fee_list_tail;
    if (mp->fee_list_tail != NULL) {
      mp->fee_list_tail->next = node;
    }
    mp->fee_list_tail = node;
    if (mp->fee_list_head == NULL) {
      mp->fee_list_head = node;
    }
  } else if (curr->prev == NULL) {
    /* Insert at head */
    node->prev = NULL;
    node->next = curr;
    curr->prev = node;
    mp->fee_list_head = node;
  } else {
    /* Insert before curr */
    node->prev = curr->prev;
    node->next = curr;
    curr->prev->next = node;
    curr->prev = node;
  }

  return ECHO_OK;
}

/**
 * Remove entry from fee-sorted list.
 */
static void fee_list_remove(mempool_t *mp, mempool_entry_t *entry) {
  fee_node_t *curr = mp->fee_list_head;

  while (curr != NULL) {
    if (curr->entry == entry) {
      if (curr->prev != NULL) {
        curr->prev->next = curr->next;
      } else {
        mp->fee_list_head = curr->next;
      }

      if (curr->next != NULL) {
        curr->next->prev = curr->prev;
      } else {
        mp->fee_list_tail = curr->prev;
      }

      free(curr);
      return;
    }
    curr = curr->next;
  }
}

/*
 * ============================================================================
 * HASH TABLE OPERATIONS
 * ============================================================================
 */

/**
 * Insert entry into txid table.
 */
static void txid_table_insert(mempool_t *mp, mempool_entry_t *entry) {
  size_t idx = hash_to_index(&entry->txid, TXID_TABLE_SIZE);
  entry->next = mp->txid_table[idx];
  mp->txid_table[idx] = entry;
}

/**
 * Remove entry from txid table.
 */
static void txid_table_remove(mempool_t *mp, mempool_entry_t *entry) {
  size_t idx = hash_to_index(&entry->txid, TXID_TABLE_SIZE);

  mempool_entry_t **prev = &mp->txid_table[idx];
  mempool_entry_t *curr = mp->txid_table[idx];

  while (curr != NULL) {
    if (curr == entry) {
      *prev = curr->next;
      return;
    }
    prev = &curr->next;
    curr = curr->next;
  }
}

/**
 * Lookup entry by txid.
 */
static mempool_entry_t *txid_table_lookup(const mempool_t *mp,
                                          const hash256_t *txid) {
  size_t idx = hash_to_index(txid, TXID_TABLE_SIZE);

  mempool_entry_t *curr = mp->txid_table[idx];
  while (curr != NULL) {
    if (memcmp(&curr->txid, txid, sizeof(hash256_t)) == 0) {
      return curr;
    }
    curr = curr->next;
  }

  return NULL;
}

/**
 * Insert entry into wtxid table.
 */
static void wtxid_table_insert(mempool_t *mp, mempool_entry_t *entry) {
  size_t idx = hash_to_index(&entry->wtxid, WTXID_TABLE_SIZE);

  /* Store in prev pointer for wtxid chain (reusing the field) */
  mempool_entry_t *old_head = mp->wtxid_table[idx];
  entry->prev = old_head;
  mp->wtxid_table[idx] = entry;
}

/**
 * Remove entry from wtxid table.
 */
static void wtxid_table_remove(mempool_t *mp, mempool_entry_t *entry) {
  size_t idx = hash_to_index(&entry->wtxid, WTXID_TABLE_SIZE);

  mempool_entry_t **prev = &mp->wtxid_table[idx];
  mempool_entry_t *curr = mp->wtxid_table[idx];

  while (curr != NULL) {
    if (curr == entry) {
      *prev = curr->prev; /* Using prev as wtxid chain link */
      return;
    }
    prev = &curr->prev;
    curr = curr->prev;
  }
}

/**
 * Lookup entry by wtxid.
 */
static mempool_entry_t *wtxid_table_lookup(const mempool_t *mp,
                                           const hash256_t *wtxid) {
  size_t idx = hash_to_index(wtxid, WTXID_TABLE_SIZE);

  mempool_entry_t *curr = mp->wtxid_table[idx];
  while (curr != NULL) {
    if (memcmp(&curr->wtxid, wtxid, sizeof(hash256_t)) == 0) {
      return curr;
    }
    curr = curr->prev; /* Using prev as wtxid chain link */
  }

  return NULL;
}

/*
 * ============================================================================
 * RBF (REPLACE-BY-FEE) HELPERS
 * ============================================================================
 *
 * These helpers implement the BIP-125 validation algorithm called from the
 * conflict branch in mempool_add.  The five BIP-125 rules and their reject
 * codes are:
 *
 *   Rule 1: Every tx in the eviction set must signal RBF (directly or by
 *           inheriting from an unconfirmed ancestor).
 *           Reject: MEMPOOL_REJECT_CONFLICT
 *
 *   Rule 2: The replacement may only spend unconfirmed outputs that are
 *           already being evicted — no new unconfirmed inputs.
 *           Reject: MEMPOOL_REJECT_CONFLICT
 *
 *   Rule 3: replacement_fee >= sum(eviction_set[i]->fee).
 *           Absolute satoshi total, not fee rate.
 *           Reject: MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE
 *
 *   Rule 4: replacement_fee >= total_evicted_fees +
 *                              (MEMPOOL_RBF_INCREMENT * vsize / 1000).
 *           Covers relay bandwidth at the minimum incremental rate.
 *           Reject: MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE
 *
 *   Rule 5: |eviction_set| <= MEMPOOL_MAX_REPLACEMENT_COUNT.
 *           Reject: MEMPOOL_REJECT_RBF_TOO_MANY_REPLACED
 */

/* Stack array size for the eviction set (capped by Rule 5). */
#define MAX_EVICTION_SET MEMPOOL_MAX_REPLACEMENT_COUNT

/* NOLINTBEGIN(misc-no-recursion) - Recursion bounded by
 * MEMPOOL_MAX_ANCESTORS (25) — walks the unconfirmed ancestor chain to check
 * inherited RBF signaling per BIP-125 Rule 1. */
/**
 * Check whether a mempool entry signals RBF directly or through any
 * unconfirmed ancestor (BIP-125 Rule 1 inherited signaling).
 *
 * Parameters:
 *   mp    - Mempool (read-only)
 *   entry - Entry to check
 *
 * Returns true if this entry or any mempool ancestor signals RBF.
 */
static bool entry_signals_rbf_inherited(const mempool_t *mp,
                                        const mempool_entry_t *entry) {
  if (entry->signals_rbf) {
    return true;
  }

  /* Walk each input: if the parent is unconfirmed and itself signals
   * (directly or via its own ancestors), this entry inherits the signal. */
  for (size_t i = 0; i < entry->tx.input_count; i++) {
    const mempool_entry_t *parent =
        txid_table_lookup(mp, &entry->tx.inputs[i].prevout.txid);
    if (parent != NULL && entry_signals_rbf_inherited(mp, parent)) {
      return true;
    }
  }

  return false;
}
/* NOLINTEND(misc-no-recursion) */

/**
 * Collect the full eviction set for an RBF replacement attempt.
 *
 * The eviction set contains all direct conflicts (mempool transactions that
 * spend at least one of the same outputs as the replacement) plus all of
 * their descendants, up to MEMPOOL_MAX_REPLACEMENT_COUNT entries.
 *
 * Rule 5 is enforced during collection: if the set would exceed
 * MEMPOOL_MAX_REPLACEMENT_COUNT, the function returns ECHO_ERR_INVALID
 * with result->reason set to MEMPOOL_REJECT_RBF_TOO_MANY_REPLACED.
 *
 * Parameters:
 *   mp             - Mempool
 *   replacement    - Incoming transaction
 *   eviction_set   - Output array (caller-allocated, size MAX_EVICTION_SET)
 *   eviction_count - Output: number of entries written
 *   result         - Output: reject reason on error
 *
 * Returns ECHO_OK on success, ECHO_ERR_INVALID if Rule 5 is violated.
 */
static echo_result_t rbf_collect_eviction_set(
    mempool_t *mp, const tx_t *replacement, mempool_entry_t **eviction_set,
    size_t *eviction_count, mempool_accept_result_t *result) {
  *eviction_count = 0;

  /* Helper: add entry to set if not already present (O(n) over ≤100 items). */
#define ADD_TO_EVICTION_SET(e)                                                 \
  do {                                                                         \
    bool _already = false;                                                     \
    for (size_t _k = 0; _k < *eviction_count; _k++) {                         \
      if (eviction_set[_k] == (e)) {                                           \
        _already = true;                                                        \
        break;                                                                  \
      }                                                                         \
    }                                                                           \
    if (!_already) {                                                            \
      if (*eviction_count >= MAX_EVICTION_SET) {                               \
        if (result != NULL) {                                                   \
          result->reason = MEMPOOL_REJECT_RBF_TOO_MANY_REPLACED;              \
        }                                                                       \
        return ECHO_ERR_INVALID;                                                \
      }                                                                         \
      eviction_set[(*eviction_count)++] = (e);                                \
    }                                                                           \
  } while (0)

  /* Step 1: collect direct conflicts (transactions that spend the same
   * outputs as the replacement). */
  for (size_t i = 0; i < replacement->input_count; i++) {
    mempool_entry_t *conflict = spent_lookup(mp, &replacement->inputs[i].prevout);
    if (conflict != NULL) {
      ADD_TO_EVICTION_SET(conflict);
    }
  }

  /* Step 2: expand the set to include all descendants of every conflict.
   * Process newly added entries as we grow the array (BFS via index walk). */
  for (size_t idx = 0; idx < *eviction_count; idx++) {
    mempool_entry_t *parent = eviction_set[idx];

    /* For each output of this conflict, look up whether a child spends it. */
    for (size_t o = 0; o < parent->tx.output_count; o++) {
      outpoint_t op = {.txid = parent->txid, .vout = (uint32_t)o};
      mempool_entry_t *child = spent_lookup(mp, &op);
      if (child != NULL) {
        ADD_TO_EVICTION_SET(child);
      }
    }
  }

#undef ADD_TO_EVICTION_SET

  return ECHO_OK;
}

/**
 * Validate an incoming transaction as a BIP-125 RBF replacement and, if all
 * five rules pass, atomically evict the displaced transactions.
 *
 * BIP-125 rules enforced (in order):
 *
 *   A. Build eviction set (direct conflicts + all descendants). Rule 5 is
 *      checked here: set size must not exceed MEMPOOL_MAX_REPLACEMENT_COUNT.
 *
 *   B. Rule 1 — Every entry in the eviction set must signal RBF directly or
 *      through an unconfirmed ancestor.
 *      Reject: MEMPOOL_REJECT_CONFLICT
 *
 *   C. Rule 2 — The replacement must not introduce new unconfirmed inputs
 *      (inputs whose parent is in the mempool but NOT in the eviction set).
 *      Reject: MEMPOOL_REJECT_CONFLICT
 *
 *   D. Rule 3 — replacement_fee >= sum(eviction_set[i]->fee).
 *      Absolute satoshi total — NOT fee rate.
 *      Reject: MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE
 *
 *   E. Rule 4 — replacement_fee >= total_evicted_fees +
 *                 (MEMPOOL_RBF_INCREMENT * replacement_vsize / 1000).
 *      Replacement must cover relay bandwidth at the incremental fee rate.
 *      Reject: MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE
 *
 *   F. Atomic eviction — collect direct conflict txids BEFORE calling
 *      mempool_remove (which modifies the spent table), then remove each
 *      direct conflict; mempool_remove recursively removes descendants.
 *
 * On ECHO_OK the replacement is NOT yet inserted — the caller (mempool_add)
 * falls through to the normal insertion path.
 *
 * Parameters:
 *   mp               - Mempool
 *   replacement      - Incoming transaction
 *   replacement_fee  - Fee of the replacement (pre-computed by mempool_add)
 *   replacement_vsize - Virtual size of the replacement
 *   result           - Output: reject reason on error; conflicts_count and
 *                      first_conflict are set on both success and failure
 *
 * Returns ECHO_OK if all rules pass, ECHO_ERR_INVALID otherwise.
 */
static echo_result_t rbf_validate_replacement(
    mempool_t *mp, const tx_t *replacement, satoshi_t replacement_fee,
    size_t replacement_vsize, mempool_accept_result_t *result) {

  /* --- Step A: Build eviction set (also checks Rule 5) --- */
  mempool_entry_t *eviction_set[MAX_EVICTION_SET];
  size_t eviction_count = 0;

  echo_result_t collect_err = rbf_collect_eviction_set(
      mp, replacement, eviction_set, &eviction_count, result);
  if (collect_err != ECHO_OK) {
    /* Rule 5 violated — result->reason already set by helper. */
    return collect_err;
  }

  /* Record result metadata (available on both success and error paths). */
  if (result != NULL) {
    result->conflicts_count = eviction_count;
    if (eviction_count > 0) {
      result->first_conflict = eviction_set[0]->txid;
    }
  }

  /* --- Step B: Rule 1 — every evicted tx must signal RBF --- */
  for (size_t i = 0; i < eviction_count; i++) {
    if (!entry_signals_rbf_inherited(mp, eviction_set[i])) {
      /* A conflicting transaction does not signal RBF and has no signaling
       * ancestor — this replacement attempt is forbidden. */
      if (result != NULL) {
        result->reason = MEMPOOL_REJECT_CONFLICT;
      }
      return ECHO_ERR_INVALID;
    }
  }

  /* --- Step C: Rule 2 — no new unconfirmed inputs --- */
  for (size_t i = 0; i < replacement->input_count; i++) {
    const mempool_entry_t *parent =
        txid_table_lookup(mp, &replacement->inputs[i].prevout.txid);
    if (parent == NULL) {
      /* Confirmed input — always allowed. */
      continue;
    }

    /* Unconfirmed parent: it must be part of the eviction set. */
    bool in_eviction_set = false;
    for (size_t k = 0; k < eviction_count; k++) {
      if (eviction_set[k] == parent) {
        in_eviction_set = true;
        break;
      }
    }

    if (!in_eviction_set) {
      /* Replacement introduces a new unconfirmed dependency not being
       * evicted — forbidden by Rule 2 to prevent "laundering" chains. */
      if (result != NULL) {
        result->reason = MEMPOOL_REJECT_CONFLICT;
      }
      return ECHO_ERR_INVALID;
    }
  }

  /* --- Step D: Rule 3 — replacement_fee >= total absolute evicted fees --- */
  satoshi_t total_evicted_fees = 0;
  for (size_t i = 0; i < eviction_count; i++) {
    total_evicted_fees += eviction_set[i]->fee;
  }

  if (replacement_fee < total_evicted_fees) {
    /* Replacement pays fewer absolute satoshis than the transactions it
     * evicts — classic "absolute fee trap" mis-implementation.  Reject. */
    if (result != NULL) {
      result->reason = MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE;
      /* Required fee: the minimum that would pass both Rule 3 and Rule 4. */
      result->required_fee = total_evicted_fees +
          (satoshi_t)((MEMPOOL_RBF_INCREMENT * replacement_vsize + 999) / 1000);
    }
    return ECHO_ERR_INVALID;
  }

  /* --- Step E: Rule 4 — fee rate increment --- */
  /* replacement_fee >= total_evicted_fees + (MEMPOOL_RBF_INCREMENT * vsize / 1000)
   *
   * MEMPOOL_RBF_INCREMENT is in sat/kvB; multiply by vsize then divide by 1000
   * to get the minimum additional fee covering relay bandwidth.
   * Integer arithmetic: use ceiling division to avoid rounding below 1 sat. */
  satoshi_t relay_fee =
      (satoshi_t)((MEMPOOL_RBF_INCREMENT * replacement_vsize + 999) / 1000);
  satoshi_t required_fee = total_evicted_fees + relay_fee;

  if (replacement_fee < required_fee) {
    if (result != NULL) {
      result->reason = MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE;
      result->required_fee = required_fee;
    }
    return ECHO_ERR_INVALID;
  }

  /* --- Step F: Atomic eviction --- */
  /* All 5 rules passed.  Collect direct conflict txids FIRST because
   * mempool_remove modifies the spent table and may invalidate pointers
   * to later entries if two conflicts share a common ancestor. */
  hash256_t direct_conflict_txids[MAX_EVICTION_SET];
  size_t direct_count = 0;

  for (size_t i = 0; i < eviction_count; i++) {
    /* A direct conflict is one that spends the same outpoint as the
     * replacement.  We identify it by checking whether the replacement
     * actually conflicts with it via spent_lookup.  All other eviction-set
     * members are descendants added by rbf_collect_eviction_set. */
    bool is_direct = false;
    for (size_t j = 0; j < replacement->input_count; j++) {
      mempool_entry_t *spender =
          spent_lookup(mp, &replacement->inputs[j].prevout);
      if (spender == eviction_set[i]) {
        is_direct = true;
        break;
      }
    }
    if (is_direct) {
      direct_conflict_txids[direct_count++] = eviction_set[i]->txid;
    }
  }

  /* Remove each direct conflict; mempool_remove handles recursive
   * descendant removal so descendants are evicted automatically. */
  for (size_t i = 0; i < direct_count; i++) {
    mempool_remove(mp, &direct_conflict_txids[i]);
  }

  /* Replacement will be inserted by the normal mempool_add insertion path. */
  if (result != NULL) {
    result->reason = MEMPOOL_ACCEPT_OK;
  }
  return ECHO_OK;
}

/*
 * ============================================================================
 * INTERNAL REMOVAL
 * ============================================================================
 */

/**
 * Remove entry from all data structures (internal).
 */
static void mempool_remove_entry(mempool_t *mp, mempool_entry_t *entry) {
  /* Remove spent outpoint tracking */
  for (size_t i = 0; i < entry->tx.input_count; i++) {
    spent_remove(mp, &entry->tx.inputs[i].prevout);
  }

  /* Remove from hash tables */
  txid_table_remove(mp, entry);
  wtxid_table_remove(mp, entry);

  /* Remove from fee list */
  fee_list_remove(mp, entry);

  /* Update statistics */
  mp->tx_count--;
  mp->total_bytes -= tx_serialize_size(&entry->tx, entry->tx.has_witness);
  mp->total_vsize -= entry->vsize;
  mp->total_fees -= entry->fee;

  /* Destroy entry */
  entry_destroy(entry);
}

/*
 * ============================================================================
 * PUBLIC API: LIFECYCLE
 * ============================================================================
 */

mempool_t *mempool_create(void) {
  mempool_config_t config = {.max_size = MEMPOOL_DEFAULT_MAX_SIZE,
                             .min_fee_rate = MEMPOOL_DEFAULT_MIN_FEE_RATE,
                             .expiry_time = MEMPOOL_DEFAULT_EXPIRY_TIME,
                             .max_ancestors = MEMPOOL_MAX_ANCESTORS,
                             .max_descendants = MEMPOOL_MAX_DESCENDANTS,
                             .max_ancestor_size = MEMPOOL_MAX_ANCESTOR_SIZE,
                             .max_descendant_size =
                                 MEMPOOL_MAX_DESCENDANT_SIZE};

  return mempool_create_with_config(&config);
}

mempool_t *mempool_create_with_config(const mempool_config_t *config) {
  if (config == NULL) {
    return NULL;
  }

  mempool_t *mp = calloc(1, sizeof(mempool_t));
  if (mp == NULL) {
    return NULL;
  }

  mp->config = *config;
  mp->has_callbacks = false;

  return mp;
}

void mempool_destroy(mempool_t *mp) {
  if (mp == NULL) {
    return;
  }

  /* Clear all entries */
  mempool_clear(mp);

  free(mp);
}

void mempool_set_callbacks(mempool_t *mp,
                           const mempool_callbacks_t *callbacks) {
  if (mp == NULL || callbacks == NULL) {
    return;
  }

  mp->callbacks = *callbacks;
  mp->has_callbacks = true;
}

/*
 * ============================================================================
 * PUBLIC API: TRANSACTION OPERATIONS
 * ============================================================================
 */

echo_result_t mempool_add(mempool_t *mp, const tx_t *tx,
                          mempool_accept_result_t *result) {
  if (mp == NULL || tx == NULL) {
    if (result != NULL) {
      result->reason = MEMPOOL_REJECT_INVALID;
    }
    return ECHO_ERR_NULL_PARAM;
  }

  /* Initialize result */
  if (result != NULL) {
    mempool_accept_result_init(result);
  }

  /* Compute txid for duplicate check */
  hash256_t txid;
  tx_compute_txid(tx, &txid);

  /* Check for duplicate */
  if (txid_table_lookup(mp, &txid) != NULL) {
    if (result != NULL) {
      result->reason = MEMPOOL_REJECT_DUPLICATE;
    }
    return ECHO_ERR_DUPLICATE;
  }

  /* Basic syntactic validation */
  tx_validate_result_t val_result;
  tx_validate_result_init(&val_result);
  echo_result_t val_err = tx_validate_syntax(tx, &val_result);
  if (val_err != ECHO_OK) {
    if (result != NULL) {
      result->reason = MEMPOOL_REJECT_INVALID;
    }
    return ECHO_ERR_INVALID;
  }

  /* Check for coinbase (can't be in mempool) */
  if (tx_is_coinbase(tx)) {
    if (result != NULL) {
      result->reason = MEMPOOL_REJECT_INVALID;
    }
    return ECHO_ERR_INVALID;
  }

  /* Calculate fee by looking up inputs */
  satoshi_t total_in = 0;
  satoshi_t total_out = 0;

  /* Sum outputs */
  for (size_t i = 0; i < tx->output_count; i++) {
    total_out += tx->outputs[i].value;
  }

  /* Track conflicts and ancestors */
  size_t ancestor_count = 1;
  size_t ancestor_size = tx_vsize(tx);
  satoshi_t ancestor_fees = 0;
  bool has_conflict = false;

  /* Look up inputs */
  for (size_t i = 0; i < tx->input_count; i++) {
    const outpoint_t *prevout = &tx->inputs[i].prevout;

    /* Check if spent by mempool tx (conflict detection).
     * We only record the first conflicting txid here; the full Rule 1
     * signaling check (including inherited signaling) happens in
     * rbf_validate_replacement() once we know a conflict exists. */
    mempool_entry_t *spender = spent_lookup(mp, prevout);
    if (spender != NULL) {
      has_conflict = true;
    }

    /* Check if input is from mempool tx (unconfirmed parent) */
    mempool_entry_t *parent = txid_table_lookup(mp, &prevout->txid);
    if (parent != NULL) {
      /* Input from mempool - add to ancestors */
      if (prevout->vout < parent->tx.output_count) {
        total_in += parent->tx.outputs[prevout->vout].value;
      }

      ancestor_count += parent->ancestor_count;
      ancestor_size += parent->ancestor_size;
      ancestor_fees += parent->ancestor_fees;
    } else if (mp->has_callbacks) {
      /* Look up from UTXO set */
      utxo_entry_t utxo;
      echo_result_t utxo_err =
          mp->callbacks.get_utxo(prevout, &utxo, mp->callbacks.ctx);
      if (utxo_err != ECHO_OK) {
        if (result != NULL) {
          result->reason = MEMPOOL_REJECT_MISSING_INPUTS;
        }
        return ECHO_ERR_NOT_FOUND;
      }

      total_in += utxo.value;

      /* Check coinbase maturity */
      if (utxo.is_coinbase) {
        uint32_t height = mp->callbacks.get_height(mp->callbacks.ctx);
        if (!utxo_entry_is_mature(&utxo, height)) {
          if (result != NULL) {
            result->reason = MEMPOOL_REJECT_PREMATURE_SPEND;
          }
          /* Free script if allocated */
          free(utxo.script_pubkey);
          return ECHO_ERR_INVALID;
        }
      }

      /* Free UTXO script */
      free(utxo.script_pubkey);
    } else {
      /* No callbacks - can't verify inputs */
      if (result != NULL) {
        result->reason = MEMPOOL_REJECT_MISSING_INPUTS;
      }
      return ECHO_ERR_NOT_FOUND;
    }
  }

  /* Calculate fee */
  if (total_in < total_out) {
    if (result != NULL) {
      result->reason = MEMPOOL_REJECT_INVALID;
    }
    return ECHO_ERR_INVALID;
  }
  satoshi_t fee = total_in - total_out;
  ancestor_fees += fee;

  /* Check fee rate */
  size_t vsize = tx_vsize(tx);
  uint64_t fee_rate = mempool_calc_fee_rate(fee, vsize);
  uint64_t min_rate = mempool_min_fee_rate(mp);

  if (fee_rate < min_rate) {
    if (result != NULL) {
      result->reason = MEMPOOL_REJECT_FEE_TOO_LOW;
      result->required_fee = (satoshi_t)((min_rate * vsize + 999) / 1000);
    }
    return ECHO_ERR_INVALID;
  }

  /* Check ancestor limits */
  if (ancestor_count > mp->config.max_ancestors) {
    if (result != NULL) {
      result->reason = MEMPOOL_REJECT_TOO_MANY_ANCESTORS;
    }
    return ECHO_ERR_INVALID;
  }

  if (ancestor_size > mp->config.max_ancestor_size) {
    if (result != NULL) {
      result->reason = MEMPOOL_REJECT_ANCESTOR_SIZE;
    }
    return ECHO_ERR_INVALID;
  }

  /* Handle RBF conflicts — validate all 5 BIP-125 rules and atomically
   * evict the displaced transactions before falling through to normal
   * insertion.  rbf_validate_replacement() populates result on error. */
  if (has_conflict) {
    echo_result_t rbf_err =
        rbf_validate_replacement(mp, tx, fee, vsize, result);
    if (rbf_err != ECHO_OK) {
      return rbf_err;
    }
    /* Replacement accepted — conflicts already evicted.
     * Fall through to normal insertion path. */
  }

  /* Check mempool size - evict if needed */
  size_t tx_bytes = tx_serialize_size(tx, tx->has_witness);
  while (mp->total_bytes + tx_bytes > mp->config.max_size &&
         mp->fee_list_tail != NULL) {
    /* Evict lowest fee-rate transaction */
    mempool_entry_t *victim = mp->fee_list_tail->entry;

    /* Don't evict if victim has higher fee rate than new tx */
    if (victim->fee_rate >= fee_rate) {
      if (result != NULL) {
        result->reason = MEMPOOL_REJECT_MEMPOOL_FULL;
        result->required_fee =
            (satoshi_t)(((victim->fee_rate + 1) * vsize + 999) / 1000);
      }
      return ECHO_ERR_FULL;
    }

    mempool_remove_entry(mp, victim);
  }

  /* Get current time and height */
  uint64_t time_added = 0;
  uint32_t height_added = 0;
  if (mp->has_callbacks) {
    /* Would need a time callback - use 0 for now */
    height_added = mp->callbacks.get_height(mp->callbacks.ctx);
  }

  /* Create entry */
  mempool_entry_t *entry = entry_create(tx, fee, time_added, height_added);
  if (entry == NULL) {
    if (result != NULL) {
      result->reason = MEMPOOL_REJECT_INVALID;
    }
    return ECHO_ERR_MEMORY;
  }

  /* Set ancestor stats */
  entry->ancestor_count = ancestor_count;
  entry->ancestor_size = ancestor_size;
  entry->ancestor_fees = ancestor_fees;

  /* Insert into hash tables */
  txid_table_insert(mp, entry);
  wtxid_table_insert(mp, entry);

  /* Insert into fee list */
  echo_result_t fee_err = fee_list_insert(mp, entry);
  if (fee_err != ECHO_OK) {
    txid_table_remove(mp, entry);
    wtxid_table_remove(mp, entry);
    entry_destroy(entry);
    if (result != NULL) {
      result->reason = MEMPOOL_REJECT_INVALID;
    }
    return fee_err;
  }

  /* Track spent outpoints */
  for (size_t i = 0; i < tx->input_count; i++) {
    spent_add(mp, &tx->inputs[i].prevout, entry);
  }

  /* Update statistics */
  mp->tx_count++;
  mp->total_bytes += tx_bytes;
  mp->total_vsize += vsize;
  mp->total_fees += fee;

  /* Update descendant counts for ancestors */
  for (size_t i = 0; i < tx->input_count; i++) {
    mempool_entry_t *parent =
        txid_table_lookup(mp, &tx->inputs[i].prevout.txid);
    if (parent != NULL) {
      parent->descendant_count++;
      parent->descendant_fees += fee;
      parent->descendant_size += vsize;
    }
  }

  /* Announce to network */
  if (mp->has_callbacks && mp->callbacks.announce_tx != NULL) {
    mp->callbacks.announce_tx(&entry->txid, mp->callbacks.ctx);
  }

  if (result != NULL) {
    result->reason = MEMPOOL_ACCEPT_OK;
  }

  return ECHO_OK;
}

/* NOLINTBEGIN(misc-no-recursion) - Recursion is intentional: removing a
 * transaction must also remove all descendants (transactions spending its
 * outputs). Depth is bounded by MEMPOOL_MAX_DESCENDANTS (25). */
echo_result_t mempool_remove(mempool_t *mp, const hash256_t *txid) {
  if (mp == NULL || txid == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  mempool_entry_t *entry = txid_table_lookup(mp, txid);
  if (entry == NULL) {
    return ECHO_ERR_NOT_FOUND;
  }

  /* First remove any descendants (transactions spending this one's outputs) */
  for (size_t i = 0; i < entry->tx.output_count; i++) {
    outpoint_t op = {.txid = entry->txid, .vout = (uint32_t)i};
    mempool_entry_t *child = spent_lookup(mp, &op);
    if (child != NULL) {
      mempool_remove(mp, &child->txid);
    }
  }

  /* Update ancestor's descendant counts */
  for (size_t i = 0; i < entry->tx.input_count; i++) {
    mempool_entry_t *parent =
        txid_table_lookup(mp, &entry->tx.inputs[i].prevout.txid);
    if (parent != NULL) {
      if (parent->descendant_count > 1) {
        parent->descendant_count--;
      }
      parent->descendant_fees -= entry->fee;
      parent->descendant_size -= entry->vsize;
    }
  }

  mempool_remove_entry(mp, entry);

  return ECHO_OK;
}
/* NOLINTEND(misc-no-recursion) */

void mempool_remove_for_block(mempool_t *mp, const block_t *block) {
  if (mp == NULL || block == NULL) {
    return;
  }

  /* Remove all transactions in the block */
  for (size_t i = 0; i < block->tx_count; i++) {
    hash256_t txid;
    tx_compute_txid(&block->txs[i], &txid);
    mempool_remove(mp, &txid);
  }

  /* Remove any transactions that conflict with block transactions */
  for (size_t i = 0; i < block->tx_count; i++) {
    const tx_t *tx = &block->txs[i];
    for (size_t j = 0; j < tx->input_count; j++) {
      mempool_entry_t *conflict = spent_lookup(mp, &tx->inputs[j].prevout);
      if (conflict != NULL) {
        mempool_remove(mp, &conflict->txid);
      }
    }
  }
}

void mempool_readd_for_disconnect(mempool_t *mp, const block_t *block) {
  if (mp == NULL || block == NULL) {
    return;
  }

  /* Re-add non-coinbase transactions from disconnected block */
  for (size_t i = 1; i < block->tx_count; i++) {
    mempool_accept_result_t result;
    mempool_add(mp, &block->txs[i], &result);
    /* Ignore failures - transaction may no longer be valid */
  }
}

const mempool_entry_t *mempool_lookup(const mempool_t *mp,
                                      const hash256_t *txid) {
  if (mp == NULL || txid == NULL) {
    return NULL;
  }
  return txid_table_lookup(mp, txid);
}

const mempool_entry_t *mempool_lookup_wtxid(const mempool_t *mp,
                                            const hash256_t *wtxid) {
  if (mp == NULL || wtxid == NULL) {
    return NULL;
  }
  return wtxid_table_lookup(mp, wtxid);
}

bool mempool_exists(const mempool_t *mp, const hash256_t *txid) {
  return mempool_lookup(mp, txid) != NULL;
}

bool mempool_is_spent(const mempool_t *mp, const outpoint_t *outpoint) {
  if (mp == NULL || outpoint == NULL) {
    return false;
  }
  return spent_lookup(mp, outpoint) != NULL;
}

const mempool_entry_t *mempool_get_spender(const mempool_t *mp,
                                           const outpoint_t *outpoint) {
  if (mp == NULL || outpoint == NULL) {
    return NULL;
  }
  return spent_lookup(mp, outpoint);
}

/*
 * ============================================================================
 * PUBLIC API: STATISTICS
 * ============================================================================
 */

size_t mempool_size(const mempool_t *mp) {
  if (mp == NULL) {
    return 0;
  }
  return mp->tx_count;
}

size_t mempool_bytes(const mempool_t *mp) {
  if (mp == NULL) {
    return 0;
  }
  return mp->total_bytes;
}

uint64_t mempool_min_fee_rate(const mempool_t *mp) {
  if (mp == NULL) {
    return MEMPOOL_DEFAULT_MIN_FEE_RATE;
  }

  /* If mempool is mostly full, increase minimum fee rate */
  if (mp->total_bytes > mp->config.max_size * 9 / 10) {
    /* Find minimum fee rate that would evict something */
    if (mp->fee_list_tail != NULL) {
      return mp->fee_list_tail->entry->fee_rate + 1;
    }
  }

  return mp->config.min_fee_rate;
}

void mempool_get_stats(const mempool_t *mp, mempool_stats_t *stats) {
  if (mp == NULL || stats == NULL) {
    return;
  }

  stats->tx_count = mp->tx_count;
  stats->total_bytes = mp->total_bytes;
  stats->total_vsize = mp->total_vsize;
  stats->total_fees = mp->total_fees;
  stats->min_fee_rate = mempool_min_fee_rate(mp);

  /* Find median fee rate */
  stats->median_fee_rate = 0;
  if (mp->tx_count > 0) {
    size_t median_idx = mp->tx_count / 2;
    size_t idx = 0;
    fee_node_t *curr = mp->fee_list_head;
    while (curr != NULL && idx < median_idx) {
      idx++;
      curr = curr->next;
    }
    if (curr != NULL) {
      stats->median_fee_rate = curr->entry->fee_rate;
    }
  }

  /* Find max ancestor/descendant counts */
  stats->max_ancestor_count = 0;
  stats->max_descendant_count = 0;
  fee_node_t *curr = mp->fee_list_head;
  while (curr != NULL) {
    if (curr->entry->ancestor_count > stats->max_ancestor_count) {
      stats->max_ancestor_count = curr->entry->ancestor_count;
    }
    if (curr->entry->descendant_count > stats->max_descendant_count) {
      stats->max_descendant_count = curr->entry->descendant_count;
    }
    curr = curr->next;
  }
}

/*
 * ============================================================================
 * PUBLIC API: TRANSACTION SELECTION
 * ============================================================================
 */

mempool_iter_t *mempool_iter_by_fee(const mempool_t *mp) {
  if (mp == NULL) {
    return NULL;
  }

  mempool_iter_t *iter = malloc(sizeof(mempool_iter_t));
  if (iter == NULL) {
    return NULL;
  }

  iter->mp = mp;
  iter->current = mp->fee_list_head;

  return iter;
}

const mempool_entry_t *mempool_iter_next(mempool_iter_t *iter) {
  if (iter == NULL || iter->current == NULL) {
    return NULL;
  }

  const mempool_entry_t *entry = iter->current->entry;
  iter->current = iter->current->next;

  return entry;
}

void mempool_iter_destroy(mempool_iter_t *iter) { free(iter); }

echo_result_t mempool_select_for_block(const mempool_t *mp,
                                       const mempool_entry_t **txs,
                                       size_t max_txs, size_t max_weight,
                                       size_t *selected) {
  if (mp == NULL || txs == NULL || selected == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  *selected = 0;
  size_t total_weight = 0;

  /* Track which transactions we've selected (by txid) */
  /* Simple approach: iterate in fee order, skip if parent not selected */

  /* First pass: mark all transactions */
  fee_node_t *curr = mp->fee_list_head;
  while (curr != NULL && *selected < max_txs) {
    mempool_entry_t *entry = curr->entry;
    size_t weight = tx_weight(&entry->tx);

    if (total_weight + weight > max_weight) {
      curr = curr->next;
      continue;
    }

    /* Check if all parents are either confirmed or already selected */
    bool parents_ready = true;
    for (size_t i = 0; i < entry->tx.input_count; i++) {
      mempool_entry_t *parent =
          txid_table_lookup(mp, &entry->tx.inputs[i].prevout.txid);
      if (parent != NULL) {
        /* Parent is in mempool - check if we've already selected it */
        bool found = false;
        for (size_t j = 0; j < *selected; j++) {
          if (memcmp(&txs[j]->txid, &parent->txid, sizeof(hash256_t)) == 0) {
            found = true;
            break;
          }
        }
        if (!found) {
          parents_ready = false;
          break;
        }
      }
    }

    if (parents_ready) {
      txs[*selected] = entry;
      (*selected)++;
      total_weight += weight;
    }

    curr = curr->next;
  }

  return ECHO_OK;
}

/*
 * ============================================================================
 * PUBLIC API: MAINTENANCE
 * ============================================================================
 */

size_t mempool_expire(mempool_t *mp, uint64_t current_time) {
  if (mp == NULL) {
    return 0;
  }

  size_t evicted = 0;
  uint64_t expiry_threshold = current_time - mp->config.expiry_time;

  /* Iterate through all entries and remove expired ones */
  fee_node_t *curr = mp->fee_list_head;
  while (curr != NULL) {
    fee_node_t *next = curr->next;
    if (curr->entry->time_added > 0 &&
        curr->entry->time_added < expiry_threshold) {
      mempool_remove(mp, &curr->entry->txid);
      evicted++;
    }
    curr = next;
  }

  return evicted;
}

size_t mempool_trim(mempool_t *mp) {
  if (mp == NULL) {
    return 0;
  }

  size_t evicted = 0;

  while (mp->total_bytes > mp->config.max_size && mp->fee_list_tail != NULL) {
    mempool_entry_t *victim = mp->fee_list_tail->entry;
    mempool_remove(mp, &victim->txid);
    evicted++;
  }

  return evicted;
}

void mempool_clear(mempool_t *mp) {
  if (mp == NULL) {
    return;
  }

  /* Free all entries via fee list */
  fee_node_t *curr = mp->fee_list_head;
  while (curr != NULL) {
    fee_node_t *next = curr->next;
    entry_destroy(curr->entry);
    free(curr);
    curr = next;
  }

  /* Free spent entries */
  for (size_t i = 0; i < SPENT_TABLE_SIZE; i++) {
    spent_entry_t *entry = mp->spent_table[i];
    while (entry != NULL) {
      spent_entry_t *next = entry->next;
      free(entry);
      entry = next;
    }
    mp->spent_table[i] = NULL;
  }

  /* Clear tables */
  memset(mp->txid_table, 0, sizeof(mp->txid_table));
  memset(mp->wtxid_table, 0, sizeof(mp->wtxid_table));

  mp->fee_list_head = NULL;
  mp->fee_list_tail = NULL;

  /* Reset statistics */
  mp->tx_count = 0;
  mp->total_bytes = 0;
  mp->total_vsize = 0;
  mp->total_fees = 0;
}

/*
 * ============================================================================
 * PUBLIC API: UTILITY
 * ============================================================================
 */

const char *mempool_reject_string(mempool_reject_t reason) {
  switch (reason) {
  case MEMPOOL_ACCEPT_OK:
    return "accepted";
  case MEMPOOL_REJECT_FEE_TOO_LOW:
    return "fee too low";
  case MEMPOOL_REJECT_MEMPOOL_FULL:
    return "mempool full";
  case MEMPOOL_REJECT_TOO_MANY_ANCESTORS:
    return "too many ancestors";
  case MEMPOOL_REJECT_TOO_MANY_DESCENDANTS:
    return "too many descendants";
  case MEMPOOL_REJECT_ANCESTOR_SIZE:
    return "ancestor size limit exceeded";
  case MEMPOOL_REJECT_DESCENDANT_SIZE:
    return "descendant size limit exceeded";
  case MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE:
    return "RBF replacement fee too low";
  case MEMPOOL_REJECT_RBF_TOO_MANY_REPLACED:
    return "RBF would replace too many transactions";
  case MEMPOOL_REJECT_NONSTANDARD:
    return "non-standard transaction";
  case MEMPOOL_REJECT_CONFLICT:
    return "conflicts with existing transaction";
  case MEMPOOL_REJECT_DUPLICATE:
    return "already in mempool";
  case MEMPOOL_REJECT_CONFIRMED:
    return "already confirmed";
  case MEMPOOL_REJECT_INVALID:
    return "invalid transaction";
  case MEMPOOL_REJECT_MISSING_INPUTS:
    return "missing inputs";
  case MEMPOOL_REJECT_PREMATURE_SPEND:
    return "coinbase not mature";
  default:
    return "unknown";
  }
}

void mempool_accept_result_init(mempool_accept_result_t *result) {
  if (result == NULL) {
    return;
  }

  result->reason = MEMPOOL_ACCEPT_OK;
  result->required_fee = 0;
  result->conflicts_count = 0;
  memset(&result->first_conflict, 0, sizeof(result->first_conflict));
}

uint64_t mempool_calc_fee_rate(satoshi_t fee, size_t vsize) {
  if (vsize == 0) {
    return 0;
  }
  /* Fee rate in sat/kvB = fee * 1000 / vsize */
  return (uint64_t)fee * 1000 / vsize;
}
