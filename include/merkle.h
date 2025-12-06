/*
 * Bitcoin Echo — Merkle Tree Computation
 *
 * This header defines functions for computing Merkle roots from
 * transaction lists. Bitcoin uses a binary Merkle tree with SHA256d
 * hashing.
 *
 * Algorithm:
 *   1. Hash each transaction to get leaves (txids or wtxids)
 *   2. If odd number of elements, duplicate the last one
 *   3. Pair elements and hash each pair: SHA256d(left || right)
 *   4. Repeat until single root remains
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_MERKLE_H
#define ECHO_MERKLE_H

#include "echo_types.h"
#include "tx.h"

/*
 * Compute Merkle root from an array of 32-byte hashes.
 *
 * This is the core Merkle tree algorithm. Given N hashes (leaves),
 * computes the root by:
 *   - If N == 0, returns all zeros
 *   - If N == 1, returns the single hash
 *   - Otherwise, pairs hashes (duplicating last if odd), computes
 *     SHA256d of each pair, and recurses
 *
 * Parameters:
 *   hashes     - Array of 32-byte hashes (leaves)
 *   count      - Number of hashes
 *   root       - Output: 32-byte Merkle root
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if hashes (when count > 0) or root is NULL
 *   ECHO_ERR_OUT_OF_MEMORY if allocation fails
 */
echo_result_t merkle_root(const hash256_t *hashes, size_t count,
                          hash256_t *root);

/*
 * Compute Merkle root from an array of transactions (using txids).
 *
 * This computes the txid of each transaction, then builds the
 * Merkle tree. This is used for the block header's merkle_root field.
 *
 * Parameters:
 *   txs        - Array of transactions
 *   count      - Number of transactions
 *   root       - Output: 32-byte Merkle root
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if txs (when count > 0) or root is NULL
 *   ECHO_ERR_OUT_OF_MEMORY if allocation fails
 */
echo_result_t merkle_root_txids(const tx_t *txs, size_t count,
                                hash256_t *root);

/*
 * Compute witness commitment Merkle root (using wtxids).
 *
 * For SegWit blocks, a separate Merkle tree is computed from wtxids.
 * The coinbase wtxid is replaced with 32 zero bytes per BIP-141.
 *
 * Parameters:
 *   txs        - Array of transactions (first must be coinbase)
 *   count      - Number of transactions
 *   root       - Output: 32-byte witness Merkle root
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if txs (when count > 0) or root is NULL
 *   ECHO_ERR_OUT_OF_MEMORY if allocation fails
 */
echo_result_t merkle_root_wtxids(const tx_t *txs, size_t count,
                                 hash256_t *root);

/*
 * Compute the witness commitment hash.
 *
 * The witness commitment is: SHA256d(witness_root || witness_nonce)
 * where witness_nonce is a 32-byte value from the coinbase.
 *
 * Parameters:
 *   witness_root  - Witness Merkle root (from merkle_root_wtxids)
 *   witness_nonce - 32-byte nonce from coinbase witness
 *   commitment    - Output: 32-byte witness commitment
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if any parameter is NULL
 */
echo_result_t witness_commitment(const hash256_t *witness_root,
                                 const hash256_t *witness_nonce,
                                 hash256_t *commitment);

/*
 * Generate a Merkle proof for a transaction at a given index.
 *
 * A Merkle proof consists of sibling hashes needed to recompute
 * the root. This allows SPV clients to verify transaction inclusion
 * without downloading the full block.
 *
 * Parameters:
 *   hashes     - Array of 32-byte hashes (leaves)
 *   count      - Number of hashes
 *   index      - Index of hash to prove
 *   proof      - Output: array of sibling hashes (caller allocates)
 *   proof_len  - Output: number of hashes in proof
 *   max_proof  - Maximum proof length (caller's buffer size)
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if required parameters are NULL
 *   ECHO_ERR_OUT_OF_RANGE if index >= count
 *   ECHO_ERR_BUFFER_TOO_SMALL if proof buffer too small
 *   ECHO_ERR_OUT_OF_MEMORY if allocation fails
 */
echo_result_t merkle_proof(const hash256_t *hashes, size_t count,
                           size_t index, hash256_t *proof,
                           size_t *proof_len, size_t max_proof);

/*
 * Verify a Merkle proof.
 *
 * Given a leaf hash, its index, and a proof, verify that it
 * hashes to the expected root.
 *
 * Parameters:
 *   leaf       - The hash being proved
 *   index      - Position of leaf in original tree
 *   count      - Total number of leaves in tree
 *   proof      - Array of sibling hashes
 *   proof_len  - Number of hashes in proof
 *   root       - Expected Merkle root
 *
 * Returns:
 *   ECHO_TRUE if proof is valid
 *   ECHO_FALSE if proof is invalid or parameters are NULL
 */
echo_bool_t merkle_verify(const hash256_t *leaf, size_t index,
                          size_t count, const hash256_t *proof,
                          size_t proof_len, const hash256_t *root);

#endif /* ECHO_MERKLE_H */
