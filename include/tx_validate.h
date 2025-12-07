/*
 * Bitcoin Echo — Transaction Validation
 *
 * This module implements complete transaction validation including:
 *   - Syntactic validation (well-formed)
 *   - Size and count limits
 *   - Duplicate input detection
 *   - Output value range validation
 *   - Script validation for each input
 *   - Locktime and sequence validation
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_TX_VALIDATE_H
#define ECHO_TX_VALIDATE_H

#include "echo_types.h"
#include "tx.h"
#include "script.h"

/*
 * ============================================================================
 * VALIDATION CONSTANTS
 * ============================================================================
 */

/*
 * Maximum transaction weight (in weight units).
 * Block weight limit is 4,000,000 WU; a single tx can't exceed it.
 */
#define TX_MAX_WEIGHT  4000000

/*
 * Maximum transaction virtual size.
 */
#define TX_MAX_VSIZE   1000000

/*
 * Minimum coinbase script length (BIP-34: height in coinbase).
 */
#define COINBASE_SCRIPT_MIN_LEN  2

/*
 * Maximum coinbase script length.
 */
#define COINBASE_SCRIPT_MAX_LEN  100

/*
 * ============================================================================
 * VALIDATION RESULT STRUCTURE
 * ============================================================================
 */

/*
 * Validation error codes (specific to tx validation).
 * More specific than the general echo_result_t codes.
 */
typedef enum {
    TX_VALIDATE_OK = 0,

    /* Structural errors */
    TX_VALIDATE_ERR_NULL,
    TX_VALIDATE_ERR_EMPTY_INPUTS,
    TX_VALIDATE_ERR_EMPTY_OUTPUTS,
    TX_VALIDATE_ERR_TOO_MANY_INPUTS,
    TX_VALIDATE_ERR_TOO_MANY_OUTPUTS,
    TX_VALIDATE_ERR_SIZE_EXCEEDED,
    TX_VALIDATE_ERR_WEIGHT_EXCEEDED,

    /* Input errors */
    TX_VALIDATE_ERR_DUPLICATE_INPUT,
    TX_VALIDATE_ERR_NULL_PREVOUT,        /* Non-coinbase with null prevout */
    TX_VALIDATE_ERR_SCRIPT_SIZE,         /* scriptSig too large */

    /* Output errors */
    TX_VALIDATE_ERR_NEGATIVE_VALUE,
    TX_VALIDATE_ERR_VALUE_TOO_LARGE,     /* Single output > 21M BTC */
    TX_VALIDATE_ERR_TOTAL_OVERFLOW,      /* Sum of outputs overflows */
    TX_VALIDATE_ERR_TOTAL_TOO_LARGE,     /* Sum of outputs > 21M BTC */
    TX_VALIDATE_ERR_OUTPUT_SCRIPT_SIZE,  /* scriptPubKey too large */

    /* Coinbase errors */
    TX_VALIDATE_ERR_COINBASE_SCRIPT_SIZE,

    /* Script execution errors */
    TX_VALIDATE_ERR_SCRIPT_FAILED,
    TX_VALIDATE_ERR_SCRIPT_VERIFY,

    /* Locktime errors */
    TX_VALIDATE_ERR_LOCKTIME,

    /* Context errors (need additional info) */
    TX_VALIDATE_ERR_MISSING_UTXO,
    TX_VALIDATE_ERR_INPUT_VALUE_OVERFLOW,
    TX_VALIDATE_ERR_INSUFFICIENT_FUNDS,

} tx_validate_error_t;

/*
 * Detailed validation result.
 * Contains the specific error code and location of the error.
 */
typedef struct {
    tx_validate_error_t error;  /* Specific error code */
    size_t              index;  /* Input/output index where error occurred */
    script_error_t      script_error;  /* Script-specific error (if applicable) */
} tx_validate_result_t;

/*
 * UTXO information needed for input validation.
 * Caller must provide this for each input being validated.
 */
typedef struct {
    satoshi_t   value;          /* Value of the UTXO */
    uint8_t    *script_pubkey;  /* The scriptPubKey of the UTXO */
    size_t      script_pubkey_len;
    uint32_t    height;         /* Block height where UTXO was created */
    echo_bool_t is_coinbase;    /* Whether this UTXO came from a coinbase */
} utxo_info_t;

/*
 * Validation context.
 * Provides additional information needed for full validation.
 */
typedef struct {
    /* Block context (for locktime validation) */
    uint32_t block_height;      /* Current block height */
    uint32_t block_time;        /* Current block timestamp */
    uint32_t median_time_past;  /* Median time past (BIP-113) */

    /* UTXO information for inputs */
    const utxo_info_t *utxos;   /* Array of UTXO info, one per input */
    size_t utxo_count;

    /* Script verification flags */
    uint32_t script_flags;      /* SCRIPT_VERIFY_* flags */

} tx_validate_ctx_t;

/*
 * ============================================================================
 * VALIDATION FUNCTIONS
 * ============================================================================
 */

/*
 * Initialize validation result to success state.
 */
void tx_validate_result_init(tx_validate_result_t *result);

/*
 * Get a human-readable string for a validation error.
 *
 * Parameters:
 *   error - The validation error code
 *
 * Returns:
 *   Static string describing the error
 */
const char *tx_validate_error_string(tx_validate_error_t error);

/*
 * Perform syntactic validation of a transaction.
 *
 * This validates the transaction structure without requiring UTXO context:
 *   - Not NULL
 *   - Has inputs (unless coinbase)
 *   - Has outputs
 *   - Input/output counts within limits
 *   - No duplicate inputs
 *   - Output values in valid range
 *   - Total output value doesn't overflow
 *   - Script sizes within limits
 *   - Transaction size/weight within limits
 *
 * Parameters:
 *   tx     - Transaction to validate
 *   result - Output: detailed validation result
 *
 * Returns:
 *   ECHO_OK if transaction is syntactically valid
 *   ECHO_ERR_* otherwise (result contains details)
 */
echo_result_t tx_validate_syntax(const tx_t *tx, tx_validate_result_t *result);

/*
 * Validate a transaction with full context.
 *
 * This performs complete validation including:
 *   - All syntactic checks
 *   - Input scripts execute successfully
 *   - Total input value >= total output value
 *   - Locktime/sequence rules satisfied
 *
 * Parameters:
 *   tx     - Transaction to validate
 *   ctx    - Validation context with UTXO info and block context
 *   result - Output: detailed validation result
 *
 * Returns:
 *   ECHO_OK if transaction is valid
 *   ECHO_ERR_* otherwise (result contains details)
 */
echo_result_t tx_validate(const tx_t *tx,
                          const tx_validate_ctx_t *ctx,
                          tx_validate_result_t *result);

/*
 * Validate a single input's script execution.
 *
 * Parameters:
 *   tx          - The transaction
 *   input_index - Index of the input to validate
 *   utxo        - UTXO information for this input
 *   flags       - Script verification flags
 *   result      - Output: validation result
 *
 * Returns:
 *   ECHO_OK if input is valid
 *   ECHO_ERR_SCRIPT_* otherwise
 */
echo_result_t tx_validate_input(const tx_t *tx,
                                size_t input_index,
                                const utxo_info_t *utxo,
                                uint32_t flags,
                                tx_validate_result_t *result);

/*
 * Check if a transaction's locktime is satisfied.
 *
 * Parameters:
 *   tx           - Transaction to check
 *   block_height - Current block height
 *   block_time   - Current block timestamp (or MTP if BIP-113)
 *
 * Returns:
 *   ECHO_TRUE if locktime satisfied, ECHO_FALSE otherwise
 */
echo_bool_t tx_locktime_satisfied(const tx_t *tx,
                                   uint32_t block_height,
                                   uint32_t block_time);

/*
 * Check if an input's relative locktime (BIP-68) is satisfied.
 *
 * Parameters:
 *   tx           - Transaction containing the input
 *   input_index  - Index of the input
 *   utxo_height  - Block height where UTXO was confirmed
 *   utxo_time    - Block time where UTXO was confirmed
 *   block_height - Current block height
 *   block_time   - Current block timestamp
 *
 * Returns:
 *   ECHO_TRUE if relative locktime satisfied, ECHO_FALSE otherwise
 */
echo_bool_t tx_sequence_satisfied(const tx_t *tx,
                                   size_t input_index,
                                   uint32_t utxo_height,
                                   uint32_t utxo_time,
                                   uint32_t block_height,
                                   uint32_t block_time);

/*
 * Compute the fee of a transaction.
 *
 * Parameters:
 *   tx          - Transaction
 *   utxos       - Array of UTXO info for each input
 *   utxo_count  - Number of UTXOs (must equal input count)
 *   fee         - Output: transaction fee in satoshis
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_* if inputs don't match or values overflow
 */
echo_result_t tx_compute_fee(const tx_t *tx,
                             const utxo_info_t *utxos,
                             size_t utxo_count,
                             satoshi_t *fee);

#endif /* ECHO_TX_VALIDATE_H */
