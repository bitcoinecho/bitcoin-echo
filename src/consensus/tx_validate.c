/*
 * Bitcoin Echo — Transaction Validation
 *
 * Implements complete transaction validation as specified in the
 * Bitcoin protocol, including all consensus rules for:
 *   - Transaction structure
 *   - Input/output limits and values
 *   - Script execution
 *   - Locktime constraints
 *
 * Build once. Build right. Stop.
 */

#include "tx_validate.h"
#include "script.h"
#include "ripemd160.h"
#include <string.h>
#include <stdlib.h>

/*
 * ============================================================================
 * HELPER FUNCTIONS
 * ============================================================================
 */

/*
 * Compare two outpoints for equality.
 */
static echo_bool_t outpoints_equal(const outpoint_t *a, const outpoint_t *b)
{
    if (memcmp(a->txid.bytes, b->txid.bytes, 32) != 0) {
        return ECHO_FALSE;
    }
    return a->vout == b->vout;
}

/*
 * Check if an outpoint is null (coinbase input marker).
 */
static echo_bool_t outpoint_is_null(const outpoint_t *out)
{
    static const uint8_t zero[32] = {0};
    if (memcmp(out->txid.bytes, zero, 32) != 0) {
        return ECHO_FALSE;
    }
    return out->vout == TX_COINBASE_VOUT;
}

/*
 * Set validation result with error.
 */
static echo_result_t set_error(tx_validate_result_t *result,
                                tx_validate_error_t error,
                                size_t index)
{
    if (result) {
        result->error = error;
        result->index = index;
        result->script_error = SCRIPT_ERR_OK;
    }
    return ECHO_ERR_PARSE_FAILED;
}

/*
 * Set validation result with script error.
 */
static echo_result_t set_script_error(tx_validate_result_t *result,
                                       size_t index,
                                       script_error_t script_err)
{
    if (result) {
        result->error = TX_VALIDATE_ERR_SCRIPT_FAILED;
        result->index = index;
        result->script_error = script_err;
    }
    return ECHO_ERR_SCRIPT_ERROR;
}

/*
 * ============================================================================
 * ERROR STRING MAPPING
 * ============================================================================
 */

const char *tx_validate_error_string(tx_validate_error_t error)
{
    switch (error) {
        case TX_VALIDATE_OK:
            return "OK";
        case TX_VALIDATE_ERR_NULL:
            return "NULL transaction";
        case TX_VALIDATE_ERR_EMPTY_INPUTS:
            return "Transaction has no inputs";
        case TX_VALIDATE_ERR_EMPTY_OUTPUTS:
            return "Transaction has no outputs";
        case TX_VALIDATE_ERR_TOO_MANY_INPUTS:
            return "Too many inputs";
        case TX_VALIDATE_ERR_TOO_MANY_OUTPUTS:
            return "Too many outputs";
        case TX_VALIDATE_ERR_SIZE_EXCEEDED:
            return "Transaction size exceeded";
        case TX_VALIDATE_ERR_WEIGHT_EXCEEDED:
            return "Transaction weight exceeded";
        case TX_VALIDATE_ERR_DUPLICATE_INPUT:
            return "Duplicate input";
        case TX_VALIDATE_ERR_NULL_PREVOUT:
            return "Null prevout in non-coinbase";
        case TX_VALIDATE_ERR_SCRIPT_SIZE:
            return "ScriptSig too large";
        case TX_VALIDATE_ERR_NEGATIVE_VALUE:
            return "Negative output value";
        case TX_VALIDATE_ERR_VALUE_TOO_LARGE:
            return "Output value too large";
        case TX_VALIDATE_ERR_TOTAL_OVERFLOW:
            return "Output total overflows";
        case TX_VALIDATE_ERR_TOTAL_TOO_LARGE:
            return "Output total too large";
        case TX_VALIDATE_ERR_OUTPUT_SCRIPT_SIZE:
            return "ScriptPubKey too large";
        case TX_VALIDATE_ERR_COINBASE_SCRIPT_SIZE:
            return "Coinbase script size invalid";
        case TX_VALIDATE_ERR_SCRIPT_FAILED:
            return "Script execution failed";
        case TX_VALIDATE_ERR_SCRIPT_VERIFY:
            return "Script verification failed";
        case TX_VALIDATE_ERR_LOCKTIME:
            return "Locktime not satisfied";
        case TX_VALIDATE_ERR_MISSING_UTXO:
            return "Missing UTXO";
        case TX_VALIDATE_ERR_INPUT_VALUE_OVERFLOW:
            return "Input value overflow";
        case TX_VALIDATE_ERR_INSUFFICIENT_FUNDS:
            return "Insufficient funds";
        default:
            return "Unknown error";
    }
}

/*
 * ============================================================================
 * INITIALIZATION
 * ============================================================================
 */

void tx_validate_result_init(tx_validate_result_t *result)
{
    if (result == NULL) return;
    result->error = TX_VALIDATE_OK;
    result->index = 0;
    result->script_error = SCRIPT_ERR_OK;
}

/*
 * ============================================================================
 * SYNTACTIC VALIDATION
 * ============================================================================
 */

echo_result_t tx_validate_syntax(const tx_t *tx, tx_validate_result_t *result)
{
    if (result) {
        tx_validate_result_init(result);
    }

    /* NULL check */
    if (tx == NULL) {
        return set_error(result, TX_VALIDATE_ERR_NULL, 0);
    }

    echo_bool_t is_coinbase = tx_is_coinbase(tx);

    /* Must have outputs */
    if (tx->output_count == 0) {
        return set_error(result, TX_VALIDATE_ERR_EMPTY_OUTPUTS, 0);
    }

    /* Must have inputs (even coinbase has one input) */
    if (tx->input_count == 0) {
        return set_error(result, TX_VALIDATE_ERR_EMPTY_INPUTS, 0);
    }

    /* Input count limit */
    if (tx->input_count > TX_MAX_INPUTS) {
        return set_error(result, TX_VALIDATE_ERR_TOO_MANY_INPUTS, tx->input_count);
    }

    /* Output count limit */
    if (tx->output_count > TX_MAX_OUTPUTS) {
        return set_error(result, TX_VALIDATE_ERR_TOO_MANY_OUTPUTS, tx->output_count);
    }

    /* Check size limits */
    size_t base_size = tx_serialize_size(tx, ECHO_FALSE);
    size_t full_size = tx_serialize_size(tx, ECHO_TRUE);

    if (full_size > TX_MAX_SIZE) {
        return set_error(result, TX_VALIDATE_ERR_SIZE_EXCEEDED, 0);
    }

    /* Check weight limit */
    size_t weight = (base_size * 3) + full_size;
    if (weight > TX_MAX_WEIGHT) {
        return set_error(result, TX_VALIDATE_ERR_WEIGHT_EXCEEDED, 0);
    }

    /* Check for duplicate inputs */
    for (size_t i = 0; i < tx->input_count; i++) {
        for (size_t j = i + 1; j < tx->input_count; j++) {
            if (outpoints_equal(&tx->inputs[i].prevout, &tx->inputs[j].prevout)) {
                return set_error(result, TX_VALIDATE_ERR_DUPLICATE_INPUT, j);
            }
        }
    }

    /* Validate inputs */
    for (size_t i = 0; i < tx->input_count; i++) {
        const tx_input_t *in = &tx->inputs[i];

        /* Check for null prevout in non-coinbase */
        if (!is_coinbase && outpoint_is_null(&in->prevout)) {
            return set_error(result, TX_VALIDATE_ERR_NULL_PREVOUT, i);
        }

        /* Check scriptSig size */
        if (in->script_sig_len > TX_MAX_SCRIPT_SIZE) {
            return set_error(result, TX_VALIDATE_ERR_SCRIPT_SIZE, i);
        }
    }

    /* Coinbase-specific checks */
    if (is_coinbase) {
        const tx_input_t *cb_in = &tx->inputs[0];
        if (cb_in->script_sig_len < COINBASE_SCRIPT_MIN_LEN ||
            cb_in->script_sig_len > COINBASE_SCRIPT_MAX_LEN) {
            return set_error(result, TX_VALIDATE_ERR_COINBASE_SCRIPT_SIZE, 0);
        }
    }

    /* Validate outputs */
    satoshi_t total_output = 0;

    for (size_t i = 0; i < tx->output_count; i++) {
        const tx_output_t *out = &tx->outputs[i];

        /* Check for negative value */
        if (out->value < 0) {
            return set_error(result, TX_VALIDATE_ERR_NEGATIVE_VALUE, i);
        }

        /* Check for value exceeding max */
        if (out->value > ECHO_MAX_SATOSHIS) {
            return set_error(result, TX_VALIDATE_ERR_VALUE_TOO_LARGE, i);
        }

        /* Check for overflow */
        if (total_output > ECHO_MAX_SATOSHIS - out->value) {
            return set_error(result, TX_VALIDATE_ERR_TOTAL_OVERFLOW, i);
        }
        total_output += out->value;

        /* Check scriptPubKey size */
        if (out->script_pubkey_len > TX_MAX_SCRIPT_SIZE) {
            return set_error(result, TX_VALIDATE_ERR_OUTPUT_SCRIPT_SIZE, i);
        }
    }

    /* Check total doesn't exceed max */
    if (total_output > ECHO_MAX_SATOSHIS) {
        return set_error(result, TX_VALIDATE_ERR_TOTAL_TOO_LARGE, 0);
    }

    return ECHO_OK;
}

/*
 * ============================================================================
 * LOCKTIME VALIDATION
 * ============================================================================
 */

echo_bool_t tx_locktime_satisfied(const tx_t *tx,
                                   uint32_t block_height,
                                   uint32_t block_time)
{
    if (tx == NULL) return ECHO_FALSE;

    /* Check if all inputs have final sequence */
    echo_bool_t all_final = ECHO_TRUE;
    for (size_t i = 0; i < tx->input_count; i++) {
        if (tx->inputs[i].sequence != TX_SEQUENCE_FINAL) {
            all_final = ECHO_FALSE;
            break;
        }
    }

    /* If all sequences are final, locktime is ignored */
    if (all_final) {
        return ECHO_TRUE;
    }

    /* Check locktime against threshold */
    if (tx->locktime < LOCKTIME_THRESHOLD) {
        /* Locktime is a block height */
        return block_height >= tx->locktime;
    } else {
        /* Locktime is a Unix timestamp */
        return block_time >= tx->locktime;
    }
}

echo_bool_t tx_sequence_satisfied(const tx_t *tx,
                                   size_t input_index,
                                   uint32_t utxo_height,
                                   uint32_t utxo_time,
                                   uint32_t block_height,
                                   uint32_t block_time)
{
    if (tx == NULL || input_index >= tx->input_count) {
        return ECHO_FALSE;
    }

    /* BIP-68 only applies to version 2+ transactions */
    if (tx->version < 2) {
        return ECHO_TRUE;
    }

    uint32_t sequence = tx->inputs[input_index].sequence;

    /* Check if relative locktime is disabled */
    if (sequence & SEQUENCE_LOCKTIME_DISABLE_FLAG) {
        return ECHO_TRUE;
    }

    uint32_t locktime_value = sequence & SEQUENCE_LOCKTIME_MASK;

    if (sequence & SEQUENCE_LOCKTIME_TYPE_FLAG) {
        /* Time-based locktime (512-second units) */
        uint32_t required_time = utxo_time + (locktime_value << 9);
        return block_time >= required_time;
    } else {
        /* Block-based locktime */
        uint32_t required_height = utxo_height + locktime_value;
        return block_height >= required_height;
    }
}

/*
 * ============================================================================
 * FEE COMPUTATION
 * ============================================================================
 */

echo_result_t tx_compute_fee(const tx_t *tx,
                             const utxo_info_t *utxos,
                             size_t utxo_count,
                             satoshi_t *fee)
{
    if (tx == NULL || fee == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    /* Coinbase has no fee (creates coins) */
    if (tx_is_coinbase(tx)) {
        *fee = 0;
        return ECHO_OK;
    }

    if (utxos == NULL || utxo_count != tx->input_count) {
        return ECHO_ERR_INVALID_FORMAT;
    }

    /* Sum input values */
    satoshi_t total_input = 0;
    for (size_t i = 0; i < tx->input_count; i++) {
        if (utxos[i].value < 0 || utxos[i].value > ECHO_MAX_SATOSHIS) {
            return ECHO_ERR_TX_NEGATIVE_VALUE;
        }
        if (total_input > ECHO_MAX_SATOSHIS - utxos[i].value) {
            return ECHO_ERR_TX_VALUE_OVERFLOW;
        }
        total_input += utxos[i].value;
    }

    /* Sum output values */
    satoshi_t total_output = 0;
    for (size_t i = 0; i < tx->output_count; i++) {
        if (tx->outputs[i].value < 0) {
            return ECHO_ERR_TX_NEGATIVE_VALUE;
        }
        total_output += tx->outputs[i].value;
    }

    /* Fee = input - output */
    if (total_input < total_output) {
        return ECHO_ERR_TX_VALUE_OVERFLOW;  /* Would be negative fee */
    }

    *fee = total_input - total_output;
    return ECHO_OK;
}

/*
 * ============================================================================
 * INPUT VALIDATION (SCRIPT EXECUTION)
 * ============================================================================
 */

echo_result_t tx_validate_input(const tx_t *tx,
                                size_t input_index,
                                const utxo_info_t *utxo,
                                uint32_t flags,
                                tx_validate_result_t *result)
{
    if (result) {
        tx_validate_result_init(result);
    }

    if (tx == NULL || utxo == NULL) {
        return set_error(result, TX_VALIDATE_ERR_NULL, input_index);
    }

    if (input_index >= tx->input_count) {
        return set_error(result, TX_VALIDATE_ERR_NULL, input_index);
    }

    const tx_input_t *input = &tx->inputs[input_index];

    /* Set up script context */
    script_context_t ctx;
    script_context_init(&ctx, flags);

    /* Set transaction context */
    script_set_tx_context(&ctx, tx, input_index, utxo->value,
                          utxo->script_pubkey, utxo->script_pubkey_len);

    /* Execute scriptSig */
    echo_result_t res = script_execute(&ctx, input->script_sig, input->script_sig_len);
    if (res != ECHO_OK) {
        script_error_t err = ctx.error;
        script_context_free(&ctx);
        return set_script_error(result, input_index, err);
    }

    /* Clear altstack between scriptSig and scriptPubKey */
    stack_free(&ctx.altstack);
    stack_init(&ctx.altstack);

    /* Handle P2SH */
    uint8_t redeem_script[TX_MAX_SCRIPT_SIZE];
    size_t redeem_len = 0;
    echo_bool_t is_p2sh = ECHO_FALSE;
    hash160_t p2sh_hash;

    if ((flags & SCRIPT_VERIFY_P2SH) &&
        script_is_p2sh(utxo->script_pubkey, utxo->script_pubkey_len, &p2sh_hash)) {
        is_p2sh = ECHO_TRUE;

        /* Check push-only for P2SH */
        if (!script_is_push_only(input->script_sig, input->script_sig_len)) {
            script_context_free(&ctx);
            return set_script_error(result, input_index, SCRIPT_ERR_SIG_PUSHONLY);
        }

        /* Save top stack element (redeem script) */
        const stack_element_t *top;
        if (stack_peek(&ctx.stack, &top) == ECHO_OK && top->len < TX_MAX_SCRIPT_SIZE) {
            memcpy(redeem_script, top->data, top->len);
            redeem_len = top->len;
        }
    }

    /* Execute scriptPubKey */
    res = script_execute(&ctx, utxo->script_pubkey, utxo->script_pubkey_len);
    if (res != ECHO_OK) {
        script_error_t err = ctx.error;
        script_context_free(&ctx);
        return set_script_error(result, input_index, err);
    }

    /* P2SH: execute redeem script */
    if (is_p2sh && redeem_len > 0) {
        /* Verify hash matches */
        uint8_t script_hash[20];
        hash160(redeem_script, redeem_len, script_hash);

        if (memcmp(script_hash, p2sh_hash.bytes, 20) == 0) {
            /* Pop the redeem script from stack */
            stack_element_t elem;
            stack_pop(&ctx.stack, &elem);
            if (elem.data) free(elem.data);

            /* Execute redeem script */
            res = script_execute(&ctx, redeem_script, redeem_len);
            if (res != ECHO_OK) {
                script_error_t err = ctx.error;
                script_context_free(&ctx);
                return set_script_error(result, input_index, err);
            }
        }
    }

    /* Check final stack state */
    if (stack_empty(&ctx.stack)) {
        script_context_free(&ctx);
        return set_script_error(result, input_index, SCRIPT_ERR_EVAL_FALSE);
    }

    const stack_element_t *top;
    if (stack_peek(&ctx.stack, &top) != ECHO_OK) {
        script_context_free(&ctx);
        return set_script_error(result, input_index, SCRIPT_ERR_EVAL_FALSE);
    }

    if (!script_bool(top->data, top->len)) {
        script_context_free(&ctx);
        return set_script_error(result, input_index, SCRIPT_ERR_EVAL_FALSE);
    }

    /* CLEANSTACK check if enabled */
    if ((flags & SCRIPT_VERIFY_CLEANSTACK) && stack_size(&ctx.stack) != 1) {
        script_context_free(&ctx);
        return set_script_error(result, input_index, SCRIPT_ERR_CLEANSTACK);
    }

    script_context_free(&ctx);
    return ECHO_OK;
}

/*
 * ============================================================================
 * FULL TRANSACTION VALIDATION
 * ============================================================================
 */

echo_result_t tx_validate(const tx_t *tx,
                          const tx_validate_ctx_t *ctx,
                          tx_validate_result_t *result)
{
    if (result) {
        tx_validate_result_init(result);
    }

    /* Syntactic validation first */
    echo_result_t res = tx_validate_syntax(tx, result);
    if (res != ECHO_OK) {
        return res;
    }

    /* Context is required for full validation */
    if (ctx == NULL) {
        return ECHO_OK;  /* Syntax-only validation passed */
    }

    echo_bool_t is_coinbase = tx_is_coinbase(tx);

    /* Coinbase doesn't need UTXO or locktime checks */
    if (is_coinbase) {
        return ECHO_OK;
    }

    /* Verify we have UTXO info for all inputs */
    if (ctx->utxos == NULL || ctx->utxo_count != tx->input_count) {
        return set_error(result, TX_VALIDATE_ERR_MISSING_UTXO, 0);
    }

    /* Check locktime */
    if (!tx_locktime_satisfied(tx, ctx->block_height,
                                ctx->median_time_past > 0 ?
                                ctx->median_time_past : ctx->block_time)) {
        return set_error(result, TX_VALIDATE_ERR_LOCKTIME, 0);
    }

    /* Sum input values and check relative locktimes */
    satoshi_t total_input = 0;

    for (size_t i = 0; i < tx->input_count; i++) {
        const utxo_info_t *utxo = &ctx->utxos[i];

        /* Check value range */
        if (utxo->value < 0 || utxo->value > ECHO_MAX_SATOSHIS) {
            return set_error(result, TX_VALIDATE_ERR_INPUT_VALUE_OVERFLOW, i);
        }

        /* Check for overflow */
        if (total_input > ECHO_MAX_SATOSHIS - utxo->value) {
            return set_error(result, TX_VALIDATE_ERR_INPUT_VALUE_OVERFLOW, i);
        }
        total_input += utxo->value;

        /* Check relative locktime (BIP-68) */
        /* Note: We'd need UTXO confirmation height/time for full check */
        /* For now, assume satisfied if context doesn't provide this info */
    }

    /* Sum output values (already checked in syntax validation) */
    satoshi_t total_output = 0;
    for (size_t i = 0; i < tx->output_count; i++) {
        total_output += tx->outputs[i].value;
    }

    /* Check total input >= total output */
    if (total_input < total_output) {
        return set_error(result, TX_VALIDATE_ERR_INSUFFICIENT_FUNDS, 0);
    }

    /* Validate each input's script */
    for (size_t i = 0; i < tx->input_count; i++) {
        res = tx_validate_input(tx, i, &ctx->utxos[i], ctx->script_flags, result);
        if (res != ECHO_OK) {
            return res;
        }
    }

    return ECHO_OK;
}
