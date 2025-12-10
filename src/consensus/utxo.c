/**
 * @file utxo.c
 * @brief UTXO set implementation
 *
 * Implements an in-memory UTXO set using a simple hash table.
 * This provides fast lookup, insertion, and removal operations.
 *
 * Implementation notes:
 * - Hash table with chaining for collision resolution
 * - Outpoint is serialized to 36 bytes for hashing
 * - Simple linear probing for now (can be optimized later if needed)
 * - All memory is explicitly managed (no hidden allocations)
 */

#include "utxo.h"
#include "echo_assert.h"
#include <stdlib.h>
#include <string.h>

/* Default hash table size (must be power of 2 for fast modulo) */
#define DEFAULT_CAPACITY 1024

/* Load factor threshold for resizing (0.75) */
#define LOAD_FACTOR_NUM 3
#define LOAD_FACTOR_DEN 4

/**
 * Hash table entry (chained)
 */
typedef struct utxo_bucket {
    utxo_entry_t *entry;        /* The UTXO entry */
    struct utxo_bucket *next;   /* Next entry in chain */
} utxo_bucket_t;

/**
 * UTXO set implementation (hash table)
 */
struct utxo_set {
    utxo_bucket_t **buckets;    /* Array of bucket chains */
    size_t capacity;            /* Number of buckets */
    size_t count;               /* Number of entries */
};

/* ========================================================================
 * Hash Function
 * ======================================================================== */

/**
 * Simple hash function for outpoints
 * Uses FNV-1a hash on the serialized outpoint (36 bytes)
 */
static uint32_t hash_outpoint(const outpoint_t *op) {
    uint8_t serialized[36];
    outpoint_serialize(op, serialized);

    /* FNV-1a hash */
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < 36; i++) {
        hash ^= serialized[i];
        hash *= 16777619u;
    }

    return hash;
}

/* ========================================================================
 * Outpoint Operations
 * ======================================================================== */

bool outpoint_equal(const outpoint_t *a, const outpoint_t *b) {
    ECHO_ASSERT(a != NULL);
    ECHO_ASSERT(b != NULL);

    if (a->vout != b->vout) {
        return false;
    }

    return memcmp(a->txid.bytes, b->txid.bytes, 32) == 0;
}

size_t outpoint_serialize(const outpoint_t *op, uint8_t *out) {
    ECHO_ASSERT(op != NULL);
    ECHO_ASSERT(out != NULL);

    /* txid (32 bytes) */
    memcpy(out, op->txid.bytes, 32);

    /* vout (4 bytes, little-endian) */
    out[32] = (op->vout >> 0) & 0xff;
    out[33] = (op->vout >> 8) & 0xff;
    out[34] = (op->vout >> 16) & 0xff;
    out[35] = (op->vout >> 24) & 0xff;

    return 36;
}

size_t outpoint_deserialize(const uint8_t *data, outpoint_t *op) {
    ECHO_ASSERT(data != NULL);
    ECHO_ASSERT(op != NULL);

    /* txid (32 bytes) */
    memcpy(op->txid.bytes, data, 32);

    /* vout (4 bytes, little-endian) */
    op->vout = ((uint32_t)data[32] << 0) |
               ((uint32_t)data[33] << 8) |
               ((uint32_t)data[34] << 16) |
               ((uint32_t)data[35] << 24);

    return 36;
}

/* ========================================================================
 * UTXO Entry Operations
 * ======================================================================== */

utxo_entry_t *utxo_entry_create(
    const outpoint_t *outpoint,
    int64_t value,
    const uint8_t *script_pubkey,
    size_t script_len,
    uint32_t height,
    bool is_coinbase
) {
    ECHO_ASSERT(outpoint != NULL);
    ECHO_ASSERT(script_pubkey != NULL);
    ECHO_ASSERT(value >= 0);

    utxo_entry_t *entry = malloc(sizeof(utxo_entry_t));
    if (entry == NULL) {
        return NULL;
    }

    entry->outpoint = *outpoint;
    entry->value = value;
    entry->script_len = script_len;
    entry->height = height;
    entry->is_coinbase = is_coinbase;

    /* Allocate and copy scriptPubKey */
    entry->script_pubkey = malloc(script_len);
    if (entry->script_pubkey == NULL) {
        free(entry);
        return NULL;
    }
    memcpy(entry->script_pubkey, script_pubkey, script_len);

    return entry;
}

void utxo_entry_destroy(utxo_entry_t *entry) {
    if (entry == NULL) {
        return;
    }

    free(entry->script_pubkey);
    free(entry);
}

utxo_entry_t *utxo_entry_clone(const utxo_entry_t *entry) {
    ECHO_ASSERT(entry != NULL);

    return utxo_entry_create(
        &entry->outpoint,
        entry->value,
        entry->script_pubkey,
        entry->script_len,
        entry->height,
        entry->is_coinbase
    );
}

bool utxo_entry_is_mature(const utxo_entry_t *entry, uint32_t current_height) {
    ECHO_ASSERT(entry != NULL);

    if (!entry->is_coinbase) {
        return true;  /* Non-coinbase outputs are always spendable */
    }

    /* Coinbase outputs require 100 confirmations */
    const uint32_t COINBASE_MATURITY = 100;

    if (current_height < entry->height) {
        return false;  /* Invalid state */
    }

    uint32_t depth = current_height - entry->height;
    return depth >= COINBASE_MATURITY;
}

/* ========================================================================
 * UTXO Set Operations
 * ======================================================================== */

utxo_set_t *utxo_set_create(size_t initial_capacity) {
    if (initial_capacity == 0) {
        initial_capacity = DEFAULT_CAPACITY;
    }

    /* Ensure capacity is power of 2 for fast modulo */
    size_t capacity = 1;
    while (capacity < initial_capacity) {
        capacity *= 2;
    }

    utxo_set_t *set = malloc(sizeof(utxo_set_t));
    if (set == NULL) {
        return NULL;
    }

    set->buckets = calloc(capacity, sizeof(utxo_bucket_t *));
    if (set->buckets == NULL) {
        free(set);
        return NULL;
    }

    set->capacity = capacity;
    set->count = 0;

    return set;
}

void utxo_set_destroy(utxo_set_t *set) {
    if (set == NULL) {
        return;
    }

    /* Free all buckets and their chains */
    for (size_t i = 0; i < set->capacity; i++) {
        utxo_bucket_t *bucket = set->buckets[i];
        while (bucket != NULL) {
            utxo_bucket_t *next = bucket->next;
            utxo_entry_destroy(bucket->entry);
            free(bucket);
            bucket = next;
        }
    }

    free(set->buckets);
    free(set);
}

size_t utxo_set_size(const utxo_set_t *set) {
    ECHO_ASSERT(set != NULL);
    return set->count;
}

const utxo_entry_t *utxo_set_lookup(
    const utxo_set_t *set,
    const outpoint_t *outpoint
) {
    ECHO_ASSERT(set != NULL);
    ECHO_ASSERT(outpoint != NULL);

    uint32_t hash = hash_outpoint(outpoint);
    size_t index = hash & (set->capacity - 1);  /* Fast modulo for power of 2 */

    utxo_bucket_t *bucket = set->buckets[index];
    while (bucket != NULL) {
        if (outpoint_equal(&bucket->entry->outpoint, outpoint)) {
            return bucket->entry;
        }
        bucket = bucket->next;
    }

    return NULL;
}

bool utxo_set_exists(const utxo_set_t *set, const outpoint_t *outpoint) {
    return utxo_set_lookup(set, outpoint) != NULL;
}

/**
 * Internal helper: resize hash table when load factor is exceeded
 */
static echo_result_t utxo_set_resize(utxo_set_t *set, size_t new_capacity) {
    ECHO_ASSERT(set != NULL);
    ECHO_ASSERT(new_capacity > set->capacity);

    /* Allocate new bucket array */
    utxo_bucket_t **new_buckets = calloc(new_capacity, sizeof(utxo_bucket_t *));
    if (new_buckets == NULL) {
        return ECHO_ERR_NOMEM;
    }

    /* Rehash all entries */
    for (size_t i = 0; i < set->capacity; i++) {
        utxo_bucket_t *bucket = set->buckets[i];
        while (bucket != NULL) {
            utxo_bucket_t *next = bucket->next;

            /* Compute new index */
            uint32_t hash = hash_outpoint(&bucket->entry->outpoint);
            size_t new_index = hash & (new_capacity - 1);

            /* Insert into new bucket array */
            bucket->next = new_buckets[new_index];
            new_buckets[new_index] = bucket;

            bucket = next;
        }
    }

    /* Replace old buckets */
    free(set->buckets);
    set->buckets = new_buckets;
    set->capacity = new_capacity;

    return ECHO_OK;
}

echo_result_t utxo_set_insert(utxo_set_t *set, const utxo_entry_t *entry) {
    ECHO_ASSERT(set != NULL);
    ECHO_ASSERT(entry != NULL);

    /* Check if already exists */
    if (utxo_set_exists(set, &entry->outpoint)) {
        return ECHO_ERR_EXISTS;
    }

    /* Check if we need to resize */
    if (set->count * LOAD_FACTOR_DEN >= set->capacity * LOAD_FACTOR_NUM) {
        echo_result_t result = utxo_set_resize(set, set->capacity * 2);
        if (result != ECHO_OK) {
            return result;
        }
    }

    /* Clone the entry */
    utxo_entry_t *new_entry = utxo_entry_clone(entry);
    if (new_entry == NULL) {
        return ECHO_ERR_NOMEM;
    }

    /* Create bucket */
    utxo_bucket_t *bucket = malloc(sizeof(utxo_bucket_t));
    if (bucket == NULL) {
        utxo_entry_destroy(new_entry);
        return ECHO_ERR_NOMEM;
    }

    bucket->entry = new_entry;

    /* Insert at head of chain */
    uint32_t hash = hash_outpoint(&entry->outpoint);
    size_t index = hash & (set->capacity - 1);

    bucket->next = set->buckets[index];
    set->buckets[index] = bucket;
    set->count++;

    return ECHO_OK;
}

echo_result_t utxo_set_remove(utxo_set_t *set, const outpoint_t *outpoint) {
    ECHO_ASSERT(set != NULL);
    ECHO_ASSERT(outpoint != NULL);

    uint32_t hash = hash_outpoint(outpoint);
    size_t index = hash & (set->capacity - 1);

    utxo_bucket_t *bucket = set->buckets[index];
    utxo_bucket_t *prev = NULL;

    while (bucket != NULL) {
        if (outpoint_equal(&bucket->entry->outpoint, outpoint)) {
            /* Found it - remove from chain */
            if (prev == NULL) {
                set->buckets[index] = bucket->next;
            } else {
                prev->next = bucket->next;
            }

            utxo_entry_destroy(bucket->entry);
            free(bucket);
            set->count--;

            return ECHO_OK;
        }

        prev = bucket;
        bucket = bucket->next;
    }

    return ECHO_ERR_NOT_FOUND;
}

void utxo_set_clear(utxo_set_t *set) {
    ECHO_ASSERT(set != NULL);

    /* Free all buckets */
    for (size_t i = 0; i < set->capacity; i++) {
        utxo_bucket_t *bucket = set->buckets[i];
        while (bucket != NULL) {
            utxo_bucket_t *next = bucket->next;
            utxo_entry_destroy(bucket->entry);
            free(bucket);
            bucket = next;
        }
        set->buckets[i] = NULL;
    }

    set->count = 0;
}

/* ========================================================================
 * Batch Operations
 * ======================================================================== */

utxo_batch_t *utxo_batch_create(void) {
    utxo_batch_t *batch = malloc(sizeof(utxo_batch_t));
    if (batch == NULL) {
        return NULL;
    }

    batch->changes = NULL;
    batch->count = 0;
    batch->capacity = 0;

    return batch;
}

void utxo_batch_destroy(utxo_batch_t *batch) {
    if (batch == NULL) {
        return;
    }

    /* Free all stored entries */
    for (size_t i = 0; i < batch->count; i++) {
        utxo_entry_destroy(batch->changes[i].entry);
    }

    free(batch->changes);
    free(batch);
}

/**
 * Internal helper: ensure batch has capacity for one more change
 */
static echo_result_t utxo_batch_ensure_capacity(utxo_batch_t *batch) {
    if (batch->count < batch->capacity) {
        return ECHO_OK;
    }

    size_t new_capacity = (batch->capacity == 0) ? 16 : batch->capacity * 2;
    utxo_change_t *new_changes = realloc(
        batch->changes,
        new_capacity * sizeof(utxo_change_t)
    );

    if (new_changes == NULL) {
        return ECHO_ERR_NOMEM;
    }

    batch->changes = new_changes;
    batch->capacity = new_capacity;

    return ECHO_OK;
}

echo_result_t utxo_batch_insert(utxo_batch_t *batch, const utxo_entry_t *entry) {
    ECHO_ASSERT(batch != NULL);
    ECHO_ASSERT(entry != NULL);

    echo_result_t result = utxo_batch_ensure_capacity(batch);
    if (result != ECHO_OK) {
        return result;
    }

    /* Record insertion (previous entry is NULL) */
    batch->changes[batch->count].outpoint = entry->outpoint;
    batch->changes[batch->count].entry = NULL;
    batch->count++;

    return ECHO_OK;
}

echo_result_t utxo_batch_remove(
    utxo_batch_t *batch,
    const outpoint_t *outpoint,
    const utxo_entry_t *old_entry
) {
    ECHO_ASSERT(batch != NULL);
    ECHO_ASSERT(outpoint != NULL);
    ECHO_ASSERT(old_entry != NULL);

    echo_result_t result = utxo_batch_ensure_capacity(batch);
    if (result != ECHO_OK) {
        return result;
    }

    /* Clone the old entry for undo */
    utxo_entry_t *cloned = utxo_entry_clone(old_entry);
    if (cloned == NULL) {
        return ECHO_ERR_NOMEM;
    }

    /* Record removal */
    batch->changes[batch->count].outpoint = *outpoint;
    batch->changes[batch->count].entry = cloned;
    batch->count++;

    return ECHO_OK;
}

echo_result_t utxo_set_apply_batch(utxo_set_t *set, const utxo_batch_t *batch) {
    ECHO_ASSERT(set != NULL);
    ECHO_ASSERT(batch != NULL);

    /* For now, we apply changes individually
     * In a real database implementation, this would be a transaction */

    for (size_t i = 0; i < batch->count; i++) {
        const utxo_change_t *change = &batch->changes[i];

        if (change->entry == NULL) {
            /* This was an insertion - the entry should now exist */
            /* Note: In real usage, the caller must provide the entry separately */
            /* For now, this is a placeholder */
            continue;
        } else {
            /* This was a removal - remove the entry */
            echo_result_t result = utxo_set_remove(set, &change->outpoint);
            if (result != ECHO_OK && result != ECHO_ERR_NOT_FOUND) {
                /* Rollback not implemented for in-memory set */
                return result;
            }
        }
    }

    return ECHO_OK;
}

echo_result_t utxo_set_revert_batch(utxo_set_t *set, const utxo_batch_t *batch) {
    ECHO_ASSERT(set != NULL);
    ECHO_ASSERT(batch != NULL);

    /* Apply changes in reverse order */
    for (size_t i = batch->count; i > 0; i--) {
        const utxo_change_t *change = &batch->changes[i - 1];

        if (change->entry == NULL) {
            /* This was an insertion - remove it */
            utxo_set_remove(set, &change->outpoint);
        } else {
            /* This was a removal - restore it */
            utxo_set_insert(set, change->entry);
        }
    }

    return ECHO_OK;
}

/* ========================================================================
 * Iteration
 * ======================================================================== */

void utxo_set_foreach(
    const utxo_set_t *set,
    utxo_iterator_fn callback,
    void *user_data
) {
    ECHO_ASSERT(set != NULL);
    ECHO_ASSERT(callback != NULL);

    for (size_t i = 0; i < set->capacity; i++) {
        utxo_bucket_t *bucket = set->buckets[i];
        while (bucket != NULL) {
            bool should_continue = callback(bucket->entry, user_data);
            if (!should_continue) {
                return;
            }
            bucket = bucket->next;
        }
    }
}
