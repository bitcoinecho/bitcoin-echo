/*
 * Bitcoin Echo — Merkle Tree Computation
 *
 * Implementation of Merkle tree algorithms for Bitcoin.
 *
 * Build once. Build right. Stop.
 */

#include "merkle.h"
#include "sha256.h"
#include <stdlib.h>
#include <string.h>

/*
 * Helper: compute SHA256d of two concatenated hashes.
 */
static void hash_pair(const hash256_t *left, const hash256_t *right,
                      hash256_t *out)
{
    uint8_t combined[64];

    memcpy(combined, left->bytes, 32);
    memcpy(combined + 32, right->bytes, 32);

    sha256d(combined, 64, out->bytes);
}

/*
 * Compute Merkle root from an array of 32-byte hashes.
 */
echo_result_t merkle_root(const hash256_t *hashes, size_t count,
                          hash256_t *root)
{
    hash256_t *current;
    hash256_t *next;
    size_t current_count;
    size_t next_count;
    size_t i;

    if (root == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    /* Empty tree: return all zeros */
    if (count == 0) {
        memset(root->bytes, 0, 32);
        return ECHO_OK;
    }

    /* Single element: return it */
    if (count == 1) {
        if (hashes == NULL) {
            return ECHO_ERR_NULL_PARAM;
        }
        memcpy(root->bytes, hashes[0].bytes, 32);
        return ECHO_OK;
    }

    if (hashes == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    /* Allocate working buffer for current layer */
    current = malloc(count * sizeof(hash256_t));
    if (current == NULL) {
        return ECHO_ERR_OUT_OF_MEMORY;
    }

    /* Copy leaves to working buffer */
    memcpy(current, hashes, count * sizeof(hash256_t));
    current_count = count;

    /* Build tree bottom-up */
    while (current_count > 1) {
        /* Calculate next layer size */
        next_count = (current_count + 1) / 2;

        /* Allocate next layer */
        next = malloc(next_count * sizeof(hash256_t));
        if (next == NULL) {
            free(current);
            return ECHO_ERR_OUT_OF_MEMORY;
        }

        /* Hash pairs */
        for (i = 0; i < next_count; i++) {
            size_t left_idx = i * 2;
            size_t right_idx = left_idx + 1;

            /* If odd number, duplicate the last element */
            if (right_idx >= current_count) {
                right_idx = left_idx;
            }

            hash_pair(&current[left_idx], &current[right_idx], &next[i]);
        }

        /* Move to next layer */
        free(current);
        current = next;
        current_count = next_count;
    }

    /* Copy root */
    memcpy(root->bytes, current[0].bytes, 32);
    free(current);

    return ECHO_OK;
}

/*
 * Compute Merkle root from an array of transactions (using txids).
 */
echo_result_t merkle_root_txids(const tx_t *txs, size_t count,
                                hash256_t *root)
{
    hash256_t *txids;
    size_t i;
    echo_result_t result;

    if (root == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    if (count == 0) {
        memset(root->bytes, 0, 32);
        return ECHO_OK;
    }

    if (txs == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    /* Allocate array for txids */
    txids = malloc(count * sizeof(hash256_t));
    if (txids == NULL) {
        return ECHO_ERR_OUT_OF_MEMORY;
    }

    /* Compute txid for each transaction */
    for (i = 0; i < count; i++) {
        result = tx_compute_txid(&txs[i], &txids[i]);
        if (result != ECHO_OK) {
            free(txids);
            return result;
        }
    }

    /* Compute Merkle root */
    result = merkle_root(txids, count, root);
    free(txids);

    return result;
}

/*
 * Compute witness commitment Merkle root (using wtxids).
 */
echo_result_t merkle_root_wtxids(const tx_t *txs, size_t count,
                                 hash256_t *root)
{
    hash256_t *wtxids;
    size_t i;
    echo_result_t result;

    if (root == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    if (count == 0) {
        memset(root->bytes, 0, 32);
        return ECHO_OK;
    }

    if (txs == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    /* Allocate array for wtxids */
    wtxids = malloc(count * sizeof(hash256_t));
    if (wtxids == NULL) {
        return ECHO_ERR_OUT_OF_MEMORY;
    }

    /* First transaction (coinbase) uses all zeros per BIP-141 */
    memset(wtxids[0].bytes, 0, 32);

    /* Compute wtxid for remaining transactions */
    for (i = 1; i < count; i++) {
        result = tx_compute_wtxid(&txs[i], &wtxids[i]);
        if (result != ECHO_OK) {
            free(wtxids);
            return result;
        }
    }

    /* Compute Merkle root */
    result = merkle_root(wtxids, count, root);
    free(wtxids);

    return result;
}

/*
 * Compute the witness commitment hash.
 */
echo_result_t witness_commitment(const hash256_t *witness_root,
                                 const hash256_t *witness_nonce,
                                 hash256_t *commitment)
{
    if (witness_root == NULL || witness_nonce == NULL || commitment == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    hash_pair(witness_root, witness_nonce, commitment);

    return ECHO_OK;
}

/*
 * Helper: count number of bits needed to represent a number.
 * This gives us the tree height.
 */
static size_t tree_height(size_t count)
{
    size_t height = 0;
    size_t n = count;

    if (n == 0) return 0;
    if (n == 1) return 0;

    n--;
    while (n > 0) {
        height++;
        n >>= 1;
    }

    return height;
}

/*
 * Generate a Merkle proof for a transaction at a given index.
 */
echo_result_t merkle_proof(const hash256_t *hashes, size_t count,
                           size_t index, hash256_t *proof,
                           size_t *proof_len, size_t max_proof)
{
    hash256_t *current;
    size_t current_count;
    size_t current_idx;
    size_t depth;
    size_t height;
    size_t i;

    if (proof_len == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    *proof_len = 0;

    if (count == 0) {
        return ECHO_OK;
    }

    if (hashes == NULL || proof == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    if (index >= count) {
        return ECHO_ERR_OUT_OF_RANGE;
    }

    /* Single element: no proof needed */
    if (count == 1) {
        return ECHO_OK;
    }

    /* Calculate tree height */
    height = tree_height(count);
    if (height > max_proof) {
        return ECHO_ERR_BUFFER_TOO_SMALL;
    }

    /* Allocate working buffer */
    current = malloc(count * sizeof(hash256_t));
    if (current == NULL) {
        return ECHO_ERR_OUT_OF_MEMORY;
    }

    memcpy(current, hashes, count * sizeof(hash256_t));
    current_count = count;
    current_idx = index;
    depth = 0;

    /* Build proof by collecting sibling at each level */
    while (current_count > 1) {
        size_t sibling_idx;
        size_t next_count = (current_count + 1) / 2;
        hash256_t *next;

        /* Find sibling */
        if (current_idx % 2 == 0) {
            /* Left node: sibling is on right */
            sibling_idx = current_idx + 1;
            if (sibling_idx >= current_count) {
                /* Odd count: duplicate self */
                sibling_idx = current_idx;
            }
        } else {
            /* Right node: sibling is on left */
            sibling_idx = current_idx - 1;
        }

        /* Add sibling to proof */
        memcpy(proof[depth].bytes, current[sibling_idx].bytes, 32);
        depth++;

        /* Build next layer */
        next = malloc(next_count * sizeof(hash256_t));
        if (next == NULL) {
            free(current);
            return ECHO_ERR_OUT_OF_MEMORY;
        }

        for (i = 0; i < next_count; i++) {
            size_t left = i * 2;
            size_t right = left + 1;
            if (right >= current_count) {
                right = left;
            }
            hash_pair(&current[left], &current[right], &next[i]);
        }

        free(current);
        current = next;
        current_count = next_count;
        current_idx /= 2;
    }

    free(current);
    *proof_len = depth;

    return ECHO_OK;
}

/*
 * Verify a Merkle proof.
 */
echo_bool_t merkle_verify(const hash256_t *leaf, size_t index,
                          size_t count, const hash256_t *proof,
                          size_t proof_len, const hash256_t *root)
{
    hash256_t current;
    size_t current_count;
    size_t current_idx;
    size_t i;

    if (leaf == NULL || root == NULL) {
        return ECHO_FALSE;
    }

    if (count == 0) {
        return ECHO_FALSE;
    }

    if (index >= count) {
        return ECHO_FALSE;
    }

    /* Single element: leaf should equal root */
    if (count == 1) {
        if (proof_len != 0) {
            return ECHO_FALSE;
        }
        return memcmp(leaf->bytes, root->bytes, 32) == 0 ?
               ECHO_TRUE : ECHO_FALSE;
    }

    if (proof == NULL && proof_len > 0) {
        return ECHO_FALSE;
    }

    /* Start with leaf */
    memcpy(current.bytes, leaf->bytes, 32);
    current_count = count;
    current_idx = index;

    /* Work up the tree */
    for (i = 0; i < proof_len; i++) {
        if (current_count <= 1) {
            return ECHO_FALSE; /* Proof too long */
        }

        if (current_idx % 2 == 0) {
            /* We're the left node */
            size_t sibling_idx = current_idx + 1;
            if (sibling_idx >= current_count) {
                /* Odd count: sibling is self (duplicated) */
                hash_pair(&current, &current, &current);
            } else {
                hash_pair(&current, &proof[i], &current);
            }
        } else {
            /* We're the right node */
            hash_pair(&proof[i], &current, &current);
        }

        current_count = (current_count + 1) / 2;
        current_idx /= 2;
    }

    /* Should have reached the root */
    if (current_count != 1) {
        return ECHO_FALSE;
    }

    return memcmp(current.bytes, root->bytes, 32) == 0 ?
           ECHO_TRUE : ECHO_FALSE;
}
