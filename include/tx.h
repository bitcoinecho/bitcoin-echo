/*
 * Bitcoin Echo — Transaction Data Structures
 *
 * This header defines the structures for Bitcoin transactions, including
 * support for both legacy and SegWit (BIP-141) formats.
 *
 * Transaction structure (legacy):
 *   - version (4 bytes, signed)
 *   - input count (varint)
 *   - inputs (variable)
 *   - output count (varint)
 *   - outputs (variable)
 *   - locktime (4 bytes)
 *
 * Transaction structure (SegWit):
 *   - version (4 bytes)
 *   - marker (0x00)
 *   - flag (0x01)
 *   - input count (varint)
 *   - inputs (variable)
 *   - output count (varint)
 *   - outputs (variable)
 *   - witness data (variable)
 *   - locktime (4 bytes)
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_TX_H
#define ECHO_TX_H

#include "echo_types.h"
#include "sha256.h"

/*
 * Transaction version constants.
 */
#define TX_VERSION_1  1
#define TX_VERSION_2  2  /* BIP-68 relative locktime */

/*
 * Sequence number constants.
 */
#define TX_SEQUENCE_FINAL           0xFFFFFFFF
#define TX_SEQUENCE_DISABLE_LOCKTIME 0xFFFFFFFF
#define TX_SEQUENCE_DISABLE_RBF     0xFFFFFFFE

/*
 * BIP-68 relative locktime sequence constants.
 * Sequence numbers are interpreted as relative locktimes when:
 *   - Transaction version >= 2
 *   - Bit 31 (SEQUENCE_LOCKTIME_DISABLE_FLAG) is NOT set
 */
#define SEQUENCE_LOCKTIME_DISABLE_FLAG  (1U << 31)  /* Bit 31: disable relative locktime */
#define SEQUENCE_LOCKTIME_TYPE_FLAG     (1U << 22)  /* Bit 22: 0=blocks, 1=time (512s units) */
#define SEQUENCE_LOCKTIME_MASK          0x0000FFFF  /* Bits 0-15: locktime value */

/*
 * Locktime threshold.
 * Values below this are block heights, values >= are Unix timestamps.
 */
#define LOCKTIME_THRESHOLD  500000000

/*
 * Coinbase constants.
 */
#define TX_COINBASE_VOUT  0xFFFFFFFF  /* Previous output index for coinbase */

/*
 * Size limits (consensus).
 */
#define TX_MAX_SIZE           4000000  /* Max transaction size in bytes */
#define TX_MAX_INPUTS         100000   /* Practical limit */
#define TX_MAX_OUTPUTS        100000   /* Practical limit */
#define TX_MAX_SCRIPT_SIZE    10000    /* Max script size */
#define TX_MAX_WITNESS_SIZE   4000000  /* Max witness size */

/*
 * Outpoint — reference to a previous transaction output.
 * Used in transaction inputs to identify which output is being spent.
 */
typedef struct {
    hash256_t txid;      /* Transaction ID (SHA256d of tx without witness) */
    uint32_t  vout;      /* Output index within that transaction */
} outpoint_t;

/*
 * Witness item — single item in a witness stack.
 */
typedef struct {
    uint8_t *data;       /* Witness item data (owned, must be freed) */
    size_t   len;        /* Length of data */
} witness_item_t;

/*
 * Witness stack — all witness items for a single input.
 */
typedef struct {
    witness_item_t *items;  /* Array of witness items (owned) */
    size_t          count;  /* Number of items */
} witness_stack_t;

/*
 * Transaction input.
 */
typedef struct {
    outpoint_t      prevout;    /* Reference to output being spent */
    uint8_t        *script_sig; /* Unlocking script (owned, must be freed) */
    size_t          script_sig_len;
    uint32_t        sequence;   /* Sequence number */
    witness_stack_t witness;    /* Witness data (empty for non-SegWit) */
} tx_input_t;

/*
 * Transaction output.
 */
typedef struct {
    satoshi_t  value;           /* Amount in satoshis */
    uint8_t   *script_pubkey;   /* Locking script (owned, must be freed) */
    size_t     script_pubkey_len;
} tx_output_t;

/*
 * Complete transaction.
 * Uses named struct for forward declaration compatibility.
 */
typedef struct tx_s {
    int32_t      version;       /* Transaction version (signed per protocol) */
    tx_input_t  *inputs;        /* Array of inputs (owned) */
    size_t       input_count;
    tx_output_t *outputs;       /* Array of outputs (owned) */
    size_t       output_count;
    uint32_t     locktime;      /* Lock time */
    echo_bool_t  has_witness;   /* True if any input has witness data */
} tx_t;

/*
 * Initialize a transaction structure to empty/safe state.
 *
 * Parameters:
 *   tx - Transaction to initialize
 */
void tx_init(tx_t *tx);

/*
 * Free all memory owned by a transaction.
 *
 * Parameters:
 *   tx - Transaction to free (structure itself is not freed)
 */
void tx_free(tx_t *tx);

/*
 * Parse a transaction from raw bytes.
 *
 * This function allocates memory for scripts and witness data.
 * Call tx_free() when done.
 *
 * Parameters:
 *   data     - Raw transaction bytes
 *   data_len - Length of data
 *   tx       - Output: parsed transaction
 *   consumed - Output: bytes consumed (may be NULL)
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if data or tx is NULL
 *   ECHO_ERR_TRUNCATED if data too short
 *   ECHO_ERR_INVALID_FORMAT if transaction malformed
 *   ECHO_ERR_OUT_OF_MEMORY if allocation fails
 */
echo_result_t tx_parse(const uint8_t *data, size_t data_len,
                       tx_t *tx, size_t *consumed);

/*
 * Compute the serialized size of a transaction.
 *
 * Parameters:
 *   tx           - Transaction to measure
 *   with_witness - Include witness data in size
 *
 * Returns:
 *   Serialized size in bytes, or 0 if tx is NULL
 */
size_t tx_serialize_size(const tx_t *tx, echo_bool_t with_witness);

/*
 * Serialize a transaction to bytes.
 *
 * Parameters:
 *   tx           - Transaction to serialize
 *   with_witness - Include witness data
 *   buf          - Output buffer
 *   buf_len      - Size of output buffer
 *   written      - Output: bytes written (may be NULL)
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if tx or buf is NULL
 *   ECHO_ERR_BUFFER_TOO_SMALL if buffer insufficient
 */
echo_result_t tx_serialize(const tx_t *tx, echo_bool_t with_witness,
                           uint8_t *buf, size_t buf_len, size_t *written);

/*
 * Compute the transaction ID (txid).
 *
 * The txid is SHA256d of the transaction serialized WITHOUT witness data.
 * This ensures txid stability for SegWit transactions.
 *
 * Parameters:
 *   tx   - Transaction
 *   txid - Output: 32-byte transaction ID
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if tx or txid is NULL
 */
echo_result_t tx_compute_txid(const tx_t *tx, hash256_t *txid);

/*
 * Compute the witness transaction ID (wtxid).
 *
 * The wtxid is SHA256d of the full transaction including witness data.
 * For non-witness transactions, wtxid equals txid.
 *
 * Parameters:
 *   tx    - Transaction
 *   wtxid - Output: 32-byte witness transaction ID
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if tx or wtxid is NULL
 */
echo_result_t tx_compute_wtxid(const tx_t *tx, hash256_t *wtxid);

/*
 * Check if a transaction is a coinbase transaction.
 *
 * A coinbase has exactly one input with null prevout (all zeros txid,
 * vout = 0xFFFFFFFF).
 *
 * Parameters:
 *   tx - Transaction to check
 *
 * Returns:
 *   ECHO_TRUE if coinbase, ECHO_FALSE otherwise
 */
echo_bool_t tx_is_coinbase(const tx_t *tx);

/*
 * Compute transaction weight (for block weight limit).
 *
 * Weight = (base size * 3) + total size
 * Where base size excludes witness data.
 *
 * Parameters:
 *   tx - Transaction
 *
 * Returns:
 *   Transaction weight in weight units, or 0 if tx is NULL
 */
size_t tx_weight(const tx_t *tx);

/*
 * Compute transaction virtual size (vsize).
 *
 * vsize = ceil(weight / 4)
 *
 * Parameters:
 *   tx - Transaction
 *
 * Returns:
 *   Virtual size in vbytes, or 0 if tx is NULL
 */
size_t tx_vsize(const tx_t *tx);

#endif /* ECHO_TX_H */
