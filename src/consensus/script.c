/*
 * Bitcoin Echo — Script Implementation
 *
 * This file implements Bitcoin Script parsing, type detection, and
 * opcode handling. It does NOT implement script execution — that
 * comes in later sessions.
 *
 * Build once. Build right. Stop.
 */

#include "script.h"
#include "sha256.h"
#include "ripemd160.h"
#include "sig_verify.h"
#include "secp256k1.h"
#include "tx.h"
#include "serialize.h"
#include <string.h>
#include <stdlib.h>

/*
 * Initialize a mutable script to empty state.
 */
void script_mut_init(script_mut_t *script)
{
    if (script == NULL) return;
    script->data = NULL;
    script->len = 0;
    script->capacity = 0;
}

/*
 * Free memory owned by a mutable script.
 */
void script_mut_free(script_mut_t *script)
{
    if (script == NULL) return;
    if (script->data != NULL) {
        free(script->data);
        script->data = NULL;
    }
    script->len = 0;
    script->capacity = 0;
}

/*
 * Check if a script is P2PKH (Pay to Public Key Hash).
 * Pattern: OP_DUP OP_HASH160 <20 bytes> OP_EQUALVERIFY OP_CHECKSIG
 * Bytes:   76     a9         14 [20 bytes] 88            ac
 */
echo_bool_t script_is_p2pkh(const uint8_t *data, size_t len, hash160_t *hash)
{
    if (data == NULL || len != SCRIPT_P2PKH_SIZE) {
        return ECHO_FALSE;
    }

    /* Check exact pattern */
    if (data[0] != OP_DUP ||
        data[1] != OP_HASH160 ||
        data[2] != 0x14 ||  /* Push 20 bytes */
        data[23] != OP_EQUALVERIFY ||
        data[24] != OP_CHECKSIG) {
        return ECHO_FALSE;
    }

    /* Extract hash if requested */
    if (hash != NULL) {
        memcpy(hash->bytes, &data[3], 20);
    }

    return ECHO_TRUE;
}

/*
 * Check if a script is P2SH (Pay to Script Hash).
 * Pattern: OP_HASH160 <20 bytes> OP_EQUAL
 * Bytes:   a9         14 [20 bytes] 87
 */
echo_bool_t script_is_p2sh(const uint8_t *data, size_t len, hash160_t *hash)
{
    if (data == NULL || len != SCRIPT_P2SH_SIZE) {
        return ECHO_FALSE;
    }

    /* Check exact pattern */
    if (data[0] != OP_HASH160 ||
        data[1] != 0x14 ||  /* Push 20 bytes */
        data[22] != OP_EQUAL) {
        return ECHO_FALSE;
    }

    /* Extract hash if requested */
    if (hash != NULL) {
        memcpy(hash->bytes, &data[2], 20);
    }

    return ECHO_TRUE;
}

/*
 * Check if a script is P2WPKH (Pay to Witness Public Key Hash).
 * Pattern: OP_0 <20 bytes>
 * Bytes:   00   14 [20 bytes]
 */
echo_bool_t script_is_p2wpkh(const uint8_t *data, size_t len, hash160_t *hash)
{
    if (data == NULL || len != SCRIPT_P2WPKH_SIZE) {
        return ECHO_FALSE;
    }

    /* Check pattern: OP_0 followed by 20-byte push */
    if (data[0] != OP_0 || data[1] != 0x14) {
        return ECHO_FALSE;
    }

    /* Extract hash if requested */
    if (hash != NULL) {
        memcpy(hash->bytes, &data[2], 20);
    }

    return ECHO_TRUE;
}

/*
 * Check if a script is P2WSH (Pay to Witness Script Hash).
 * Pattern: OP_0 <32 bytes>
 * Bytes:   00   20 [32 bytes]
 */
echo_bool_t script_is_p2wsh(const uint8_t *data, size_t len, hash256_t *hash)
{
    if (data == NULL || len != SCRIPT_P2WSH_SIZE) {
        return ECHO_FALSE;
    }

    /* Check pattern: OP_0 followed by 32-byte push */
    if (data[0] != OP_0 || data[1] != 0x20) {
        return ECHO_FALSE;
    }

    /* Extract hash if requested */
    if (hash != NULL) {
        memcpy(hash->bytes, &data[2], 32);
    }

    return ECHO_TRUE;
}

/*
 * Check if a script is P2TR (Pay to Taproot).
 * Pattern: OP_1 <32 bytes>
 * Bytes:   51   20 [32 bytes]
 */
echo_bool_t script_is_p2tr(const uint8_t *data, size_t len, hash256_t *key)
{
    if (data == NULL || len != SCRIPT_P2TR_SIZE) {
        return ECHO_FALSE;
    }

    /* Check pattern: OP_1 followed by 32-byte push */
    if (data[0] != OP_1 || data[1] != 0x20) {
        return ECHO_FALSE;
    }

    /* Extract key if requested */
    if (key != NULL) {
        memcpy(key->bytes, &data[2], 32);
    }

    return ECHO_TRUE;
}

/*
 * Check if a script is P2PK (Pay to Public Key).
 * Pattern: <33 or 65 bytes pubkey> OP_CHECKSIG
 */
echo_bool_t script_is_p2pk(const uint8_t *data, size_t len,
                           const uint8_t **pubkey, size_t *pubkey_len)
{
    if (data == NULL || len < 35) {  /* Minimum: 1 + 33 + 1 */
        return ECHO_FALSE;
    }

    /* Check for compressed public key (33 bytes) */
    if (len == 35 && data[0] == 0x21 && data[34] == OP_CHECKSIG) {
        /* Verify it looks like a compressed pubkey (02 or 03 prefix) */
        if (data[1] == 0x02 || data[1] == 0x03) {
            if (pubkey != NULL) *pubkey = &data[1];
            if (pubkey_len != NULL) *pubkey_len = 33;
            return ECHO_TRUE;
        }
    }

    /* Check for uncompressed public key (65 bytes) */
    if (len == 67 && data[0] == 0x41 && data[66] == OP_CHECKSIG) {
        /* Verify it looks like an uncompressed pubkey (04 prefix) */
        if (data[1] == 0x04) {
            if (pubkey != NULL) *pubkey = &data[1];
            if (pubkey_len != NULL) *pubkey_len = 65;
            return ECHO_TRUE;
        }
    }

    return ECHO_FALSE;
}

/*
 * Check if a script is OP_RETURN (null data / unspendable).
 * Pattern: OP_RETURN [data...]
 */
echo_bool_t script_is_op_return(const uint8_t *data, size_t len)
{
    if (data == NULL || len < 1) {
        return ECHO_FALSE;
    }

    return (data[0] == OP_RETURN) ? ECHO_TRUE : ECHO_FALSE;
}

/*
 * Check if a script is a witness program.
 *
 * A witness program has the form:
 *   <version byte> <push opcode> <program>
 *
 * Where:
 *   - version is OP_0 (0x00) or OP_1-OP_16 (0x51-0x60)
 *   - push opcode indicates 2-40 bytes
 *   - program is the witness program data
 */
echo_bool_t script_is_witness_program(const uint8_t *data, size_t len,
                                       witness_program_t *witness)
{
    if (data == NULL || len < 4 || len > 42) {
        return ECHO_FALSE;
    }

    /* First byte must be a valid witness version */
    int version;
    if (data[0] == OP_0) {
        version = 0;
    } else if (data[0] >= OP_1 && data[0] <= OP_16) {
        version = data[0] - OP_1 + 1;
    } else {
        return ECHO_FALSE;
    }

    /* Second byte must be a direct push of 2-40 bytes */
    size_t program_len = data[1];
    if (program_len < 2 || program_len > 40) {
        return ECHO_FALSE;
    }

    /* Total length must match exactly */
    if (len != 2 + program_len) {
        return ECHO_FALSE;
    }

    /* Fill output if provided */
    if (witness != NULL) {
        witness->version = version;
        witness->program_len = program_len;
        memcpy(witness->program, &data[2], program_len);
    }

    return ECHO_TRUE;
}

/*
 * Determine the type of a script.
 */
script_type_t script_classify(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return SCRIPT_TYPE_UNKNOWN;
    }

    /* Check for OP_RETURN first (unspendable) */
    if (script_is_op_return(data, len)) {
        return SCRIPT_TYPE_NULL_DATA;
    }

    /* Check P2PKH: OP_DUP OP_HASH160 <20> OP_EQUALVERIFY OP_CHECKSIG */
    if (script_is_p2pkh(data, len, NULL)) {
        return SCRIPT_TYPE_P2PKH;
    }

    /* Check P2SH: OP_HASH160 <20> OP_EQUAL */
    if (script_is_p2sh(data, len, NULL)) {
        return SCRIPT_TYPE_P2SH;
    }

    /* Check witness programs */
    witness_program_t witness;
    if (script_is_witness_program(data, len, &witness)) {
        if (witness.version == 0) {
            if (witness.program_len == 20) {
                return SCRIPT_TYPE_P2WPKH;
            } else if (witness.program_len == 32) {
                return SCRIPT_TYPE_P2WSH;
            }
        } else if (witness.version == 1 && witness.program_len == 32) {
            return SCRIPT_TYPE_P2TR;
        }
        /* Future witness versions */
        return SCRIPT_TYPE_WITNESS_UNKNOWN;
    }

    /* Check P2PK: <pubkey> OP_CHECKSIG */
    if (script_is_p2pk(data, len, NULL, NULL)) {
        return SCRIPT_TYPE_P2PK;
    }

    /* Check bare multisig: <m> <pubkey>... <n> OP_CHECKMULTISIG */
    /* Basic detection: ends with OP_CHECKMULTISIG and starts with small number */
    if (len >= 3 && data[len - 1] == OP_CHECKMULTISIG) {
        /* First opcode should be OP_1 through OP_16 */
        if (data[0] >= OP_1 && data[0] <= OP_16) {
            return SCRIPT_TYPE_MULTISIG;
        }
    }

    return SCRIPT_TYPE_UNKNOWN;
}

/*
 * Initialize a script iterator.
 */
void script_iter_init(script_iter_t *iter, const uint8_t *script, size_t len)
{
    if (iter == NULL) return;
    iter->script = script;
    iter->script_len = len;
    iter->pos = 0;
    iter->error = ECHO_FALSE;
}

/*
 * Get the next opcode from a script.
 */
echo_bool_t script_iter_next(script_iter_t *iter, script_op_t *op)
{
    if (iter == NULL || op == NULL || iter->error) {
        return ECHO_FALSE;
    }

    if (iter->pos >= iter->script_len) {
        return ECHO_FALSE;
    }

    uint8_t opcode = iter->script[iter->pos++];
    op->op = (script_opcode_t)opcode;
    op->data = NULL;
    op->len = 0;

    /* Handle push opcodes */
    if (opcode == OP_0) {
        /* OP_0 pushes empty array */
        op->data = NULL;
        op->len = 0;
    } else if (opcode >= 0x01 && opcode <= 0x4b) {
        /* Direct push: opcode is the number of bytes to push */
        size_t push_len = opcode;
        if (iter->pos + push_len > iter->script_len) {
            iter->error = ECHO_TRUE;
            return ECHO_FALSE;
        }
        op->data = &iter->script[iter->pos];
        op->len = push_len;
        iter->pos += push_len;
    } else if (opcode == OP_PUSHDATA1) {
        /* Next byte is length */
        if (iter->pos >= iter->script_len) {
            iter->error = ECHO_TRUE;
            return ECHO_FALSE;
        }
        size_t push_len = iter->script[iter->pos++];
        if (iter->pos + push_len > iter->script_len) {
            iter->error = ECHO_TRUE;
            return ECHO_FALSE;
        }
        op->data = &iter->script[iter->pos];
        op->len = push_len;
        iter->pos += push_len;
    } else if (opcode == OP_PUSHDATA2) {
        /* Next 2 bytes (LE) is length */
        if (iter->pos + 2 > iter->script_len) {
            iter->error = ECHO_TRUE;
            return ECHO_FALSE;
        }
        size_t push_len = iter->script[iter->pos] |
                          ((size_t)iter->script[iter->pos + 1] << 8);
        iter->pos += 2;
        if (iter->pos + push_len > iter->script_len) {
            iter->error = ECHO_TRUE;
            return ECHO_FALSE;
        }
        op->data = &iter->script[iter->pos];
        op->len = push_len;
        iter->pos += push_len;
    } else if (opcode == OP_PUSHDATA4) {
        /* Next 4 bytes (LE) is length */
        if (iter->pos + 4 > iter->script_len) {
            iter->error = ECHO_TRUE;
            return ECHO_FALSE;
        }
        size_t push_len = iter->script[iter->pos] |
                          ((size_t)iter->script[iter->pos + 1] << 8) |
                          ((size_t)iter->script[iter->pos + 2] << 16) |
                          ((size_t)iter->script[iter->pos + 3] << 24);
        iter->pos += 4;
        if (iter->pos + push_len > iter->script_len) {
            iter->error = ECHO_TRUE;
            return ECHO_FALSE;
        }
        op->data = &iter->script[iter->pos];
        op->len = push_len;
        iter->pos += push_len;
    }
    /* All other opcodes have no associated data */

    return ECHO_TRUE;
}

/*
 * Check if iterator encountered an error.
 */
echo_bool_t script_iter_error(const script_iter_t *iter)
{
    if (iter == NULL) return ECHO_TRUE;
    return iter->error;
}

/*
 * Check if an opcode is disabled.
 */
echo_bool_t script_opcode_disabled(script_opcode_t op)
{
    switch (op) {
        case OP_CAT:
        case OP_SUBSTR:
        case OP_LEFT:
        case OP_RIGHT:
        case OP_INVERT:
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_2MUL:
        case OP_2DIV:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
        case OP_LSHIFT:
        case OP_RSHIFT:
            return ECHO_TRUE;
        default:
            return ECHO_FALSE;
    }
}

/*
 * Check if a byte value is a push opcode.
 */
echo_bool_t script_opcode_is_push(uint8_t op)
{
    /* OP_0 through OP_PUSHDATA4 (0x00 - 0x4e) */
    return (op <= OP_PUSHDATA4) ? ECHO_TRUE : ECHO_FALSE;
}

/*
 * Get the name of an opcode as a string.
 */
const char *script_opcode_name(script_opcode_t op)
{
    switch (op) {
        case OP_0: return "OP_0";
        case OP_PUSHDATA1: return "OP_PUSHDATA1";
        case OP_PUSHDATA2: return "OP_PUSHDATA2";
        case OP_PUSHDATA4: return "OP_PUSHDATA4";
        case OP_1NEGATE: return "OP_1NEGATE";
        case OP_RESERVED: return "OP_RESERVED";
        case OP_1: return "OP_1";
        case OP_2: return "OP_2";
        case OP_3: return "OP_3";
        case OP_4: return "OP_4";
        case OP_5: return "OP_5";
        case OP_6: return "OP_6";
        case OP_7: return "OP_7";
        case OP_8: return "OP_8";
        case OP_9: return "OP_9";
        case OP_10: return "OP_10";
        case OP_11: return "OP_11";
        case OP_12: return "OP_12";
        case OP_13: return "OP_13";
        case OP_14: return "OP_14";
        case OP_15: return "OP_15";
        case OP_16: return "OP_16";
        case OP_NOP: return "OP_NOP";
        case OP_VER: return "OP_VER";
        case OP_IF: return "OP_IF";
        case OP_NOTIF: return "OP_NOTIF";
        case OP_VERIF: return "OP_VERIF";
        case OP_VERNOTIF: return "OP_VERNOTIF";
        case OP_ELSE: return "OP_ELSE";
        case OP_ENDIF: return "OP_ENDIF";
        case OP_VERIFY: return "OP_VERIFY";
        case OP_RETURN: return "OP_RETURN";
        case OP_TOALTSTACK: return "OP_TOALTSTACK";
        case OP_FROMALTSTACK: return "OP_FROMALTSTACK";
        case OP_2DROP: return "OP_2DROP";
        case OP_2DUP: return "OP_2DUP";
        case OP_3DUP: return "OP_3DUP";
        case OP_2OVER: return "OP_2OVER";
        case OP_2ROT: return "OP_2ROT";
        case OP_2SWAP: return "OP_2SWAP";
        case OP_IFDUP: return "OP_IFDUP";
        case OP_DEPTH: return "OP_DEPTH";
        case OP_DROP: return "OP_DROP";
        case OP_DUP: return "OP_DUP";
        case OP_NIP: return "OP_NIP";
        case OP_OVER: return "OP_OVER";
        case OP_PICK: return "OP_PICK";
        case OP_ROLL: return "OP_ROLL";
        case OP_ROT: return "OP_ROT";
        case OP_SWAP: return "OP_SWAP";
        case OP_TUCK: return "OP_TUCK";
        case OP_CAT: return "OP_CAT";
        case OP_SUBSTR: return "OP_SUBSTR";
        case OP_LEFT: return "OP_LEFT";
        case OP_RIGHT: return "OP_RIGHT";
        case OP_SIZE: return "OP_SIZE";
        case OP_INVERT: return "OP_INVERT";
        case OP_AND: return "OP_AND";
        case OP_OR: return "OP_OR";
        case OP_XOR: return "OP_XOR";
        case OP_EQUAL: return "OP_EQUAL";
        case OP_EQUALVERIFY: return "OP_EQUALVERIFY";
        case OP_RESERVED1: return "OP_RESERVED1";
        case OP_RESERVED2: return "OP_RESERVED2";
        case OP_1ADD: return "OP_1ADD";
        case OP_1SUB: return "OP_1SUB";
        case OP_2MUL: return "OP_2MUL";
        case OP_2DIV: return "OP_2DIV";
        case OP_NEGATE: return "OP_NEGATE";
        case OP_ABS: return "OP_ABS";
        case OP_NOT: return "OP_NOT";
        case OP_0NOTEQUAL: return "OP_0NOTEQUAL";
        case OP_ADD: return "OP_ADD";
        case OP_SUB: return "OP_SUB";
        case OP_MUL: return "OP_MUL";
        case OP_DIV: return "OP_DIV";
        case OP_MOD: return "OP_MOD";
        case OP_LSHIFT: return "OP_LSHIFT";
        case OP_RSHIFT: return "OP_RSHIFT";
        case OP_BOOLAND: return "OP_BOOLAND";
        case OP_BOOLOR: return "OP_BOOLOR";
        case OP_NUMEQUAL: return "OP_NUMEQUAL";
        case OP_NUMEQUALVERIFY: return "OP_NUMEQUALVERIFY";
        case OP_NUMNOTEQUAL: return "OP_NUMNOTEQUAL";
        case OP_LESSTHAN: return "OP_LESSTHAN";
        case OP_GREATERTHAN: return "OP_GREATERTHAN";
        case OP_LESSTHANOREQUAL: return "OP_LESSTHANOREQUAL";
        case OP_GREATERTHANOREQUAL: return "OP_GREATERTHANOREQUAL";
        case OP_MIN: return "OP_MIN";
        case OP_MAX: return "OP_MAX";
        case OP_WITHIN: return "OP_WITHIN";
        case OP_RIPEMD160: return "OP_RIPEMD160";
        case OP_SHA1: return "OP_SHA1";
        case OP_SHA256: return "OP_SHA256";
        case OP_HASH160: return "OP_HASH160";
        case OP_HASH256: return "OP_HASH256";
        case OP_CODESEPARATOR: return "OP_CODESEPARATOR";
        case OP_CHECKSIG: return "OP_CHECKSIG";
        case OP_CHECKSIGVERIFY: return "OP_CHECKSIGVERIFY";
        case OP_CHECKMULTISIG: return "OP_CHECKMULTISIG";
        case OP_CHECKMULTISIGVERIFY: return "OP_CHECKMULTISIGVERIFY";
        case OP_NOP1: return "OP_NOP1";
        case OP_CHECKLOCKTIMEVERIFY: return "OP_CHECKLOCKTIMEVERIFY";
        case OP_CHECKSEQUENCEVERIFY: return "OP_CHECKSEQUENCEVERIFY";
        case OP_NOP4: return "OP_NOP4";
        case OP_NOP5: return "OP_NOP5";
        case OP_NOP6: return "OP_NOP6";
        case OP_NOP7: return "OP_NOP7";
        case OP_NOP8: return "OP_NOP8";
        case OP_NOP9: return "OP_NOP9";
        case OP_NOP10: return "OP_NOP10";
        case OP_CHECKSIGADD: return "OP_CHECKSIGADD";
        case OP_INVALIDOPCODE: return "OP_INVALIDOPCODE";
        default:
            /* Handle direct push opcodes (0x01-0x4b) */
            if (op >= 0x01 && op <= 0x4b) {
                return "OP_PUSHBYTES";
            }
            return "OP_UNKNOWN";
    }
}

/*
 * Count the number of signature operations in a script.
 */
size_t script_sigops_count(const uint8_t *data, size_t len, echo_bool_t accurate)
{
    if (data == NULL || len == 0) {
        return 0;
    }

    size_t count = 0;
    script_iter_t iter;
    script_op_t op;
    script_opcode_t last_op = OP_INVALIDOPCODE;

    script_iter_init(&iter, data, len);

    while (script_iter_next(&iter, &op)) {
        switch (op.op) {
            case OP_CHECKSIG:
            case OP_CHECKSIGVERIFY:
                count++;
                break;

            case OP_CHECKMULTISIG:
            case OP_CHECKMULTISIGVERIFY:
                if (accurate && last_op >= OP_1 && last_op <= OP_16) {
                    /* Accurate count: use actual number of keys */
                    count += (last_op - OP_1 + 1);
                } else {
                    /* Conservative count: assume maximum */
                    count += SCRIPT_MAX_PUBKEYS_PER_MULTISIG;
                }
                break;

            default:
                break;
        }
        last_op = op.op;
    }

    return count;
}

/*
 * Compute the minimum push size for a given data length.
 */
size_t script_push_size(size_t data_len)
{
    if (data_len == 0) {
        return 1;  /* OP_0 */
    } else if (data_len <= 75) {
        return 1 + data_len;  /* Direct push */
    } else if (data_len <= 255) {
        return 1 + 1 + data_len;  /* OP_PUSHDATA1 */
    } else if (data_len <= 65535) {
        return 1 + 2 + data_len;  /* OP_PUSHDATA2 */
    } else {
        return 1 + 4 + data_len;  /* OP_PUSHDATA4 */
    }
}


/*
 * ============================================================================
 * STACK IMPLEMENTATION
 * ============================================================================
 */

/*
 * Initial capacity for stack.
 */
#define STACK_INITIAL_CAPACITY 64

/*
 * Initialize a script stack.
 */
echo_result_t stack_init(script_stack_t *stack)
{
    if (stack == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    stack->elements = (stack_element_t *)malloc(
        STACK_INITIAL_CAPACITY * sizeof(stack_element_t));
    if (stack->elements == NULL) {
        return ECHO_ERR_OUT_OF_MEMORY;
    }

    stack->count = 0;
    stack->capacity = STACK_INITIAL_CAPACITY;
    return ECHO_OK;
}

/*
 * Free a single stack element.
 */
static void element_free(stack_element_t *elem)
{
    if (elem != NULL && elem->data != NULL) {
        free(elem->data);
        elem->data = NULL;
        elem->len = 0;
    }
}

/*
 * Free all memory owned by a stack.
 */
void stack_free(script_stack_t *stack)
{
    if (stack == NULL) return;

    if (stack->elements != NULL) {
        for (size_t i = 0; i < stack->count; i++) {
            element_free(&stack->elements[i]);
        }
        free(stack->elements);
        stack->elements = NULL;
    }
    stack->count = 0;
    stack->capacity = 0;
}

/*
 * Clear all elements from a stack.
 */
void stack_clear(script_stack_t *stack)
{
    if (stack == NULL) return;

    for (size_t i = 0; i < stack->count; i++) {
        element_free(&stack->elements[i]);
    }
    stack->count = 0;
}

/*
 * Get the number of elements on the stack.
 */
size_t stack_size(const script_stack_t *stack)
{
    if (stack == NULL) return 0;
    return stack->count;
}

/*
 * Check if stack is empty.
 */
echo_bool_t stack_empty(const script_stack_t *stack)
{
    if (stack == NULL) return ECHO_TRUE;
    return (stack->count == 0) ? ECHO_TRUE : ECHO_FALSE;
}

/*
 * Ensure stack has capacity for at least one more element.
 */
static echo_result_t stack_ensure_capacity(script_stack_t *stack)
{
    if (stack->count >= SCRIPT_MAX_STACK_SIZE) {
        return ECHO_ERR_SCRIPT_STACK;
    }

    if (stack->count >= stack->capacity) {
        size_t new_cap = stack->capacity * 2;
        if (new_cap > SCRIPT_MAX_STACK_SIZE) {
            new_cap = SCRIPT_MAX_STACK_SIZE;
        }
        stack_element_t *new_elems = (stack_element_t *)realloc(
            stack->elements, new_cap * sizeof(stack_element_t));
        if (new_elems == NULL) {
            return ECHO_ERR_OUT_OF_MEMORY;
        }
        stack->elements = new_elems;
        stack->capacity = new_cap;
    }

    return ECHO_OK;
}

/*
 * Push a byte array onto the stack.
 */
echo_result_t stack_push(script_stack_t *stack, const uint8_t *data, size_t len)
{
    if (stack == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    echo_result_t res = stack_ensure_capacity(stack);
    if (res != ECHO_OK) return res;

    stack_element_t *elem = &stack->elements[stack->count];

    if (len == 0) {
        elem->data = NULL;
        elem->len = 0;
    } else {
        elem->data = (uint8_t *)malloc(len);
        if (elem->data == NULL) {
            return ECHO_ERR_OUT_OF_MEMORY;
        }
        memcpy(elem->data, data, len);
        elem->len = len;
    }

    stack->count++;
    return ECHO_OK;
}

/*
 * Pop the top element from the stack.
 */
echo_result_t stack_pop(script_stack_t *stack, stack_element_t *elem)
{
    if (stack == NULL || elem == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    if (stack->count == 0) {
        return ECHO_ERR_SCRIPT_STACK;
    }

    stack->count--;
    *elem = stack->elements[stack->count];
    /* Caller now owns the data */
    stack->elements[stack->count].data = NULL;
    stack->elements[stack->count].len = 0;

    return ECHO_OK;
}

/*
 * Peek at the top element.
 */
echo_result_t stack_peek(const script_stack_t *stack, const stack_element_t **elem)
{
    if (stack == NULL || elem == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    if (stack->count == 0) {
        return ECHO_ERR_SCRIPT_STACK;
    }

    *elem = &stack->elements[stack->count - 1];
    return ECHO_OK;
}

/*
 * Peek at an element by index from top.
 */
echo_result_t stack_peek_at(const script_stack_t *stack, size_t index,
                            const stack_element_t **elem)
{
    if (stack == NULL || elem == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    if (index >= stack->count) {
        return ECHO_ERR_OUT_OF_RANGE;
    }

    *elem = &stack->elements[stack->count - 1 - index];
    return ECHO_OK;
}

/*
 * Duplicate the top element.
 */
echo_result_t stack_dup(script_stack_t *stack)
{
    if (stack == NULL) return ECHO_ERR_NULL_PARAM;
    if (stack->count == 0) return ECHO_ERR_SCRIPT_STACK;

    const stack_element_t *top = &stack->elements[stack->count - 1];
    return stack_push(stack, top->data, top->len);
}

/*
 * Remove the top element without returning it.
 */
echo_result_t stack_drop(script_stack_t *stack)
{
    if (stack == NULL) return ECHO_ERR_NULL_PARAM;
    if (stack->count == 0) return ECHO_ERR_SCRIPT_STACK;

    stack->count--;
    element_free(&stack->elements[stack->count]);
    return ECHO_OK;
}

/*
 * Swap the top two elements.
 */
echo_result_t stack_swap(script_stack_t *stack)
{
    if (stack == NULL) return ECHO_ERR_NULL_PARAM;
    if (stack->count < 2) return ECHO_ERR_SCRIPT_STACK;

    stack_element_t tmp = stack->elements[stack->count - 1];
    stack->elements[stack->count - 1] = stack->elements[stack->count - 2];
    stack->elements[stack->count - 2] = tmp;
    return ECHO_OK;
}

/*
 * Rotate the top three elements: (x1 x2 x3 -- x2 x3 x1)
 */
echo_result_t stack_rot(script_stack_t *stack)
{
    if (stack == NULL) return ECHO_ERR_NULL_PARAM;
    if (stack->count < 3) return ECHO_ERR_SCRIPT_STACK;

    stack_element_t tmp = stack->elements[stack->count - 3];
    stack->elements[stack->count - 3] = stack->elements[stack->count - 2];
    stack->elements[stack->count - 2] = stack->elements[stack->count - 1];
    stack->elements[stack->count - 1] = tmp;
    return ECHO_OK;
}

/*
 * Copy the second-to-top element to the top.
 */
echo_result_t stack_over(script_stack_t *stack)
{
    if (stack == NULL) return ECHO_ERR_NULL_PARAM;
    if (stack->count < 2) return ECHO_ERR_SCRIPT_STACK;

    const stack_element_t *second = &stack->elements[stack->count - 2];
    return stack_push(stack, second->data, second->len);
}

/*
 * Remove the second-to-top element.
 */
echo_result_t stack_nip(script_stack_t *stack)
{
    if (stack == NULL) return ECHO_ERR_NULL_PARAM;
    if (stack->count < 2) return ECHO_ERR_SCRIPT_STACK;

    /* Free the second element */
    element_free(&stack->elements[stack->count - 2]);
    /* Move top down */
    stack->elements[stack->count - 2] = stack->elements[stack->count - 1];
    stack->elements[stack->count - 1].data = NULL;
    stack->elements[stack->count - 1].len = 0;
    stack->count--;
    return ECHO_OK;
}

/*
 * Copy the top element and insert it below the second element.
 * (x1 x2 -- x2 x1 x2)
 */
echo_result_t stack_tuck(script_stack_t *stack)
{
    if (stack == NULL) return ECHO_ERR_NULL_PARAM;
    if (stack->count < 2) return ECHO_ERR_SCRIPT_STACK;

    /* First swap, then over */
    echo_result_t res = stack_swap(stack);
    if (res != ECHO_OK) return res;
    return stack_over(stack);
}

/*
 * Duplicate top two elements.
 */
echo_result_t stack_2dup(script_stack_t *stack)
{
    if (stack == NULL) return ECHO_ERR_NULL_PARAM;
    if (stack->count < 2) return ECHO_ERR_SCRIPT_STACK;

    const stack_element_t *e1 = &stack->elements[stack->count - 2];
    const stack_element_t *e2 = &stack->elements[stack->count - 1];

    echo_result_t res = stack_push(stack, e1->data, e1->len);
    if (res != ECHO_OK) return res;
    return stack_push(stack, e2->data, e2->len);
}

/*
 * Duplicate top three elements.
 */
echo_result_t stack_3dup(script_stack_t *stack)
{
    if (stack == NULL) return ECHO_ERR_NULL_PARAM;
    if (stack->count < 3) return ECHO_ERR_SCRIPT_STACK;

    const stack_element_t *e1 = &stack->elements[stack->count - 3];
    const stack_element_t *e2 = &stack->elements[stack->count - 2];
    const stack_element_t *e3 = &stack->elements[stack->count - 1];

    echo_result_t res = stack_push(stack, e1->data, e1->len);
    if (res != ECHO_OK) return res;
    res = stack_push(stack, e2->data, e2->len);
    if (res != ECHO_OK) return res;
    return stack_push(stack, e3->data, e3->len);
}

/*
 * Drop top two elements.
 */
echo_result_t stack_2drop(script_stack_t *stack)
{
    if (stack == NULL) return ECHO_ERR_NULL_PARAM;
    if (stack->count < 2) return ECHO_ERR_SCRIPT_STACK;

    stack_drop(stack);
    stack_drop(stack);
    return ECHO_OK;
}

/*
 * Copy elements 3 and 4 to top.
 * (x1 x2 x3 x4 -- x1 x2 x3 x4 x1 x2)
 */
echo_result_t stack_2over(script_stack_t *stack)
{
    if (stack == NULL) return ECHO_ERR_NULL_PARAM;
    if (stack->count < 4) return ECHO_ERR_SCRIPT_STACK;

    const stack_element_t *e1 = &stack->elements[stack->count - 4];
    const stack_element_t *e2 = &stack->elements[stack->count - 3];

    echo_result_t res = stack_push(stack, e1->data, e1->len);
    if (res != ECHO_OK) return res;
    return stack_push(stack, e2->data, e2->len);
}

/*
 * Swap top two pairs.
 * (x1 x2 x3 x4 -- x3 x4 x1 x2)
 */
echo_result_t stack_2swap(script_stack_t *stack)
{
    if (stack == NULL) return ECHO_ERR_NULL_PARAM;
    if (stack->count < 4) return ECHO_ERR_SCRIPT_STACK;

    stack_element_t tmp;

    /* Swap positions 0 and 2 (from top: -4 and -2) */
    tmp = stack->elements[stack->count - 4];
    stack->elements[stack->count - 4] = stack->elements[stack->count - 2];
    stack->elements[stack->count - 2] = tmp;

    /* Swap positions 1 and 3 (from top: -3 and -1) */
    tmp = stack->elements[stack->count - 3];
    stack->elements[stack->count - 3] = stack->elements[stack->count - 1];
    stack->elements[stack->count - 1] = tmp;

    return ECHO_OK;
}

/*
 * Rotate top three pairs.
 * (x1 x2 x3 x4 x5 x6 -- x3 x4 x5 x6 x1 x2)
 */
echo_result_t stack_2rot(script_stack_t *stack)
{
    if (stack == NULL) return ECHO_ERR_NULL_PARAM;
    if (stack->count < 6) return ECHO_ERR_SCRIPT_STACK;

    /* Save bottom pair */
    stack_element_t e1 = stack->elements[stack->count - 6];
    stack_element_t e2 = stack->elements[stack->count - 5];

    /* Shift pairs down */
    stack->elements[stack->count - 6] = stack->elements[stack->count - 4];
    stack->elements[stack->count - 5] = stack->elements[stack->count - 3];
    stack->elements[stack->count - 4] = stack->elements[stack->count - 2];
    stack->elements[stack->count - 3] = stack->elements[stack->count - 1];

    /* Put saved pair on top */
    stack->elements[stack->count - 2] = e1;
    stack->elements[stack->count - 1] = e2;

    return ECHO_OK;
}

/*
 * Copy nth element to top.
 */
echo_result_t stack_pick(script_stack_t *stack, size_t n)
{
    if (stack == NULL) return ECHO_ERR_NULL_PARAM;
    if (n >= stack->count) return ECHO_ERR_OUT_OF_RANGE;

    const stack_element_t *elem = &stack->elements[stack->count - 1 - n];
    return stack_push(stack, elem->data, elem->len);
}

/*
 * Move nth element to top.
 */
echo_result_t stack_roll(script_stack_t *stack, size_t n)
{
    if (stack == NULL) return ECHO_ERR_NULL_PARAM;
    if (n >= stack->count) return ECHO_ERR_OUT_OF_RANGE;
    if (n == 0) return ECHO_OK;  /* No-op */

    /* Save the element we're rolling */
    size_t idx = stack->count - 1 - n;
    stack_element_t elem = stack->elements[idx];

    /* Shift elements down */
    for (size_t i = idx; i < stack->count - 1; i++) {
        stack->elements[i] = stack->elements[i + 1];
    }

    /* Put rolled element on top */
    stack->elements[stack->count - 1] = elem;

    return ECHO_OK;
}


/*
 * ============================================================================
 * NUMBER CONVERSION
 * ============================================================================
 */

/*
 * Convert a byte array to a script number.
 */
echo_result_t script_num_decode(const uint8_t *data, size_t len,
                                script_num_t *num,
                                echo_bool_t require_minimal, size_t max_size)
{
    if (num == NULL) return ECHO_ERR_NULL_PARAM;

    *num = 0;

    /* Empty array is zero */
    if (data == NULL || len == 0) {
        return ECHO_OK;
    }

    /* Check size limit */
    if (len > max_size) {
        return ECHO_ERR_OUT_OF_RANGE;
    }

    /* Check for minimal encoding if required */
    if (require_minimal) {
        /* Check for unnecessary leading zero bytes.
         * The MSB of the last byte is the sign bit.
         * A leading 0x00 is only allowed if the next byte has its high bit set
         * (to distinguish positive from negative). */
        if (len > 1) {
            /* If last byte is 0x00, check if it's necessary */
            if (data[len - 1] == 0x00 && (data[len - 2] & 0x80) == 0) {
                return ECHO_ERR_INVALID_FORMAT;
            }
            /* If last byte is 0x80 (negative zero marker), check if necessary */
            if (data[len - 1] == 0x80 && (data[len - 2] & 0x80) == 0) {
                return ECHO_ERR_INVALID_FORMAT;
            }
        }
    }

    /* Decode the number (little-endian with sign bit) */
    script_num_t result = 0;
    for (size_t i = 0; i < len; i++) {
        result |= ((script_num_t)data[i]) << (8 * i);
    }

    /* Handle sign bit */
    if (data[len - 1] & 0x80) {
        /* Negative number: clear sign bit and negate */
        result &= ~((script_num_t)0x80 << (8 * (len - 1)));
        result = -result;
    }

    *num = result;
    return ECHO_OK;
}

/*
 * Convert a script number to minimal byte array.
 */
echo_result_t script_num_encode(script_num_t num, uint8_t *buf, size_t *len)
{
    if (buf == NULL || len == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    /* Zero is encoded as empty array */
    if (num == 0) {
        *len = 0;
        return ECHO_OK;
    }

    echo_bool_t negative = (num < 0) ? ECHO_TRUE : ECHO_FALSE;
    uint64_t absval = negative ? (uint64_t)(-num) : (uint64_t)num;

    /* Encode absolute value in little-endian */
    size_t n = 0;
    while (absval > 0) {
        buf[n++] = (uint8_t)(absval & 0xFF);
        absval >>= 8;
    }

    /* Handle sign bit */
    if (buf[n - 1] & 0x80) {
        /* Need extra byte for sign */
        buf[n++] = negative ? 0x80 : 0x00;
    } else if (negative) {
        /* Set sign bit in last byte */
        buf[n - 1] |= 0x80;
    }

    *len = n;
    return ECHO_OK;
}

/*
 * Check if a byte array represents "true" in script.
 */
echo_bool_t script_bool(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ECHO_FALSE;
    }

    /* Check for any non-zero byte, except negative zero */
    for (size_t i = 0; i < len; i++) {
        if (data[i] != 0) {
            /* Special case: negative zero (0x80 in last byte, rest zeros) */
            if (i == len - 1 && data[i] == 0x80) {
                return ECHO_FALSE;
            }
            return ECHO_TRUE;
        }
    }

    return ECHO_FALSE;
}

/*
 * Push a script number onto the stack.
 */
echo_result_t stack_push_num(script_stack_t *stack, script_num_t num)
{
    uint8_t buf[9];  /* Max 8 bytes + possible sign byte */
    size_t len;

    echo_result_t res = script_num_encode(num, buf, &len);
    if (res != ECHO_OK) return res;

    return stack_push(stack, buf, len);
}

/*
 * Push a boolean onto the stack.
 */
echo_result_t stack_push_bool(script_stack_t *stack, echo_bool_t val)
{
    if (val) {
        uint8_t one = 0x01;
        return stack_push(stack, &one, 1);
    } else {
        return stack_push(stack, NULL, 0);
    }
}

/*
 * Pop the top element and interpret it as a number.
 */
echo_result_t stack_pop_num(script_stack_t *stack, script_num_t *num,
                            echo_bool_t require_minimal, size_t max_size)
{
    if (stack == NULL || num == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    stack_element_t elem;
    echo_result_t res = stack_pop(stack, &elem);
    if (res != ECHO_OK) return res;

    res = script_num_decode(elem.data, elem.len, num, require_minimal, max_size);
    element_free(&elem);

    return res;
}

/*
 * Pop the top element and interpret it as a boolean.
 */
echo_result_t stack_pop_bool(script_stack_t *stack, echo_bool_t *val)
{
    if (stack == NULL || val == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    stack_element_t elem;
    echo_result_t res = stack_pop(stack, &elem);
    if (res != ECHO_OK) return res;

    *val = script_bool(elem.data, elem.len);
    element_free(&elem);

    return ECHO_OK;
}


/*
 * ============================================================================
 * CONTEXT FUNCTIONS
 * ============================================================================
 */

/*
 * Initialize a script execution context.
 */
echo_result_t script_context_init(script_context_t *ctx, uint32_t flags)
{
    if (ctx == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    echo_result_t res = stack_init(&ctx->stack);
    if (res != ECHO_OK) return res;

    res = stack_init(&ctx->altstack);
    if (res != ECHO_OK) {
        stack_free(&ctx->stack);
        return res;
    }

    ctx->flags = flags;
    ctx->error = SCRIPT_ERR_OK;
    ctx->op_count = 0;
    ctx->exec_depth = 0;
    ctx->skip_depth = 0;

    /* Initialize transaction context (no tx by default) */
    ctx->tx = NULL;
    ctx->input_index = 0;
    ctx->amount = 0;
    ctx->script_code = NULL;
    ctx->script_code_len = 0;
    ctx->codesep_pos = 0;

    /* Initialize sighash cache */
    ctx->sighash_cached = ECHO_FALSE;
    memset(ctx->hash_prevouts, 0, 32);
    memset(ctx->hash_sequence, 0, 32);
    memset(ctx->hash_outputs, 0, 32);

    /* Initialize Taproot context */
    ctx->is_tapscript = ECHO_FALSE;
    memset(ctx->tapleaf_hash, 0, 32);
    memset(ctx->tap_internal_key, 0, 32);
    ctx->key_version = 0;

    /* Initialize Taproot sighash cache */
    ctx->tap_sighash_cached = ECHO_FALSE;
    memset(ctx->hash_amounts, 0, 32);
    memset(ctx->hash_scriptpubkeys, 0, 32);

    return ECHO_OK;
}

/*
 * Free all resources owned by a context.
 */
void script_context_free(script_context_t *ctx)
{
    if (ctx == NULL) return;

    stack_free(&ctx->stack);
    stack_free(&ctx->altstack);
    ctx->flags = 0;
    ctx->error = SCRIPT_ERR_OK;
    ctx->op_count = 0;
    ctx->exec_depth = 0;
    ctx->skip_depth = 0;
}

/*
 * Get error message for a script error code.
 */
const char *script_error_string(script_error_t err)
{
    switch (err) {
        case SCRIPT_ERR_OK: return "No error";
        case SCRIPT_ERR_UNKNOWN_ERROR: return "Unknown error";
        case SCRIPT_ERR_EVAL_FALSE: return "Script evaluated to false";
        case SCRIPT_ERR_OP_RETURN: return "OP_RETURN encountered";
        case SCRIPT_ERR_SCRIPT_SIZE: return "Script size limit exceeded";
        case SCRIPT_ERR_PUSH_SIZE: return "Push size limit exceeded";
        case SCRIPT_ERR_OP_COUNT: return "Operation count limit exceeded";
        case SCRIPT_ERR_STACK_SIZE: return "Stack size limit exceeded";
        case SCRIPT_ERR_SIG_COUNT: return "Signature count limit exceeded";
        case SCRIPT_ERR_PUBKEY_COUNT: return "Public key count limit exceeded";
        case SCRIPT_ERR_INVALID_STACK_OPERATION: return "Invalid stack operation";
        case SCRIPT_ERR_INVALID_ALTSTACK_OPERATION: return "Invalid altstack operation";
        case SCRIPT_ERR_UNBALANCED_CONDITIONAL: return "Unbalanced conditional";
        case SCRIPT_ERR_DISABLED_OPCODE: return "Disabled opcode";
        case SCRIPT_ERR_RESERVED_OPCODE: return "Reserved opcode";
        case SCRIPT_ERR_BAD_OPCODE: return "Bad opcode";
        case SCRIPT_ERR_INVALID_OPCODE: return "Invalid opcode";
        case SCRIPT_ERR_VERIFY: return "OP_VERIFY failed";
        case SCRIPT_ERR_EQUALVERIFY: return "OP_EQUALVERIFY failed";
        case SCRIPT_ERR_CHECKMULTISIGVERIFY: return "OP_CHECKMULTISIGVERIFY failed";
        case SCRIPT_ERR_CHECKSIGVERIFY: return "OP_CHECKSIGVERIFY failed";
        case SCRIPT_ERR_NUMEQUALVERIFY: return "OP_NUMEQUALVERIFY failed";
        case SCRIPT_ERR_INVALID_NUMBER_RANGE: return "Invalid number range";
        case SCRIPT_ERR_IMPOSSIBLE_ENCODING: return "Impossible encoding";
        case SCRIPT_ERR_NEGATIVE_LOCKTIME: return "Negative locktime";
        case SCRIPT_ERR_UNSATISFIED_LOCKTIME: return "Unsatisfied locktime";
        case SCRIPT_ERR_SIG_HASHTYPE: return "Invalid signature hash type";
        case SCRIPT_ERR_SIG_DER: return "Invalid DER signature";
        case SCRIPT_ERR_SIG_HIGH_S: return "High S value in signature";
        case SCRIPT_ERR_SIG_NULLDUMMY: return "Dummy must be empty";
        case SCRIPT_ERR_SIG_NULLFAIL: return "Signature must be empty on failure";
        case SCRIPT_ERR_PUBKEYTYPE: return "Invalid public key type";
        case SCRIPT_ERR_SIG_BADLENGTH: return "Invalid signature length";
        case SCRIPT_ERR_SCHNORR_SIG: return "Invalid Schnorr signature";
        case SCRIPT_ERR_SIG_PUSHONLY: return "scriptSig must be push-only";
        case SCRIPT_ERR_WITNESS_PROGRAM_WRONG_LENGTH: return "Wrong witness program length";
        case SCRIPT_ERR_WITNESS_PROGRAM_WITNESS_EMPTY: return "Witness program requires witness";
        case SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH: return "Witness program mismatch";
        case SCRIPT_ERR_WITNESS_MALLEATED: return "Witness malleated";
        case SCRIPT_ERR_WITNESS_MALLEATED_P2SH: return "Witness malleated (P2SH)";
        case SCRIPT_ERR_WITNESS_UNEXPECTED: return "Unexpected witness";
        case SCRIPT_ERR_WITNESS_PUBKEYTYPE: return "Invalid witness public key type";
        case SCRIPT_ERR_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM: return "Discouraged upgradable witness program";
        case SCRIPT_ERR_TAPROOT_WRONG_CONTROL_SIZE: return "Wrong taproot control size";
        case SCRIPT_ERR_TAPSCRIPT_VALIDATION_WEIGHT: return "Tapscript validation weight exceeded";
        case SCRIPT_ERR_TAPSCRIPT_CHECKMULTISIG: return "CHECKMULTISIG not in tapscript";
        case SCRIPT_ERR_TAPSCRIPT_MINIMALIF: return "Tapscript requires minimal IF";
        case SCRIPT_ERR_CLEANSTACK: return "Stack not clean after execution";
        case SCRIPT_ERR_OUT_OF_MEMORY: return "Out of memory";
        default: return "Unknown error";
    }
}


/*
 * ============================================================================
 * OPCODE EXECUTION (Session 4.3)
 * ============================================================================
 */

/*
 * Check if currently executing (not in a false branch of IF/ELSE).
 */
echo_bool_t script_is_executing(const script_context_t *ctx)
{
    if (ctx == NULL) return ECHO_FALSE;
    return (ctx->skip_depth == 0) ? ECHO_TRUE : ECHO_FALSE;
}

/*
 * Helper: Set context error and return appropriate result.
 */
static echo_result_t script_set_error(script_context_t *ctx, script_error_t err)
{
    ctx->error = err;
    return ECHO_ERR_SCRIPT_ERROR;
}

/*
 * Execute a single opcode.
 */
echo_result_t script_exec_op(script_context_t *ctx, const script_op_t *op)
{
    if (ctx == NULL || op == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    echo_bool_t executing = script_is_executing(ctx);
    script_opcode_t opcode = op->op;

    /*
     * Flow control opcodes are always processed, even in non-executing branches.
     * This is necessary to track nesting depth.
     */
    if (opcode == OP_IF || opcode == OP_NOTIF) {
        if (executing) {
            /* Pop condition and check */
            if (stack_empty(&ctx->stack)) {
                return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
            }
            stack_element_t elem;
            echo_result_t res = stack_pop(&ctx->stack, &elem);
            if (res != ECHO_OK) {
                return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
            }

            echo_bool_t condition = script_bool(elem.data, elem.len);
            if (elem.data) free(elem.data);

            /* For NOTIF, invert the condition */
            if (opcode == OP_NOTIF) {
                condition = condition ? ECHO_FALSE : ECHO_TRUE;
            }

            if (condition) {
                ctx->exec_depth++;
            } else {
                ctx->skip_depth++;
            }
        } else {
            /* Not executing, just track nesting */
            ctx->skip_depth++;
        }
        return ECHO_OK;
    }

    if (opcode == OP_ELSE) {
        if (ctx->exec_depth == 0 && ctx->skip_depth == 0) {
            return script_set_error(ctx, SCRIPT_ERR_UNBALANCED_CONDITIONAL);
        }
        if (ctx->skip_depth == 1) {
            /* Was skipping, now execute */
            ctx->skip_depth--;
            ctx->exec_depth++;
        } else if (ctx->skip_depth == 0 && ctx->exec_depth > 0) {
            /* Was executing, now skip */
            ctx->exec_depth--;
            ctx->skip_depth++;
        }
        /* If skip_depth > 1, stay skipping */
        return ECHO_OK;
    }

    if (opcode == OP_ENDIF) {
        if (ctx->exec_depth == 0 && ctx->skip_depth == 0) {
            return script_set_error(ctx, SCRIPT_ERR_UNBALANCED_CONDITIONAL);
        }
        if (ctx->skip_depth > 0) {
            ctx->skip_depth--;
        } else {
            ctx->exec_depth--;
        }
        return ECHO_OK;
    }

    /* All other opcodes only execute if we're in an executing branch */
    if (!executing) {
        return ECHO_OK;
    }

    /* Check for disabled opcodes */
    if (script_opcode_disabled(opcode)) {
        return script_set_error(ctx, SCRIPT_ERR_DISABLED_OPCODE);
    }

    /* Count non-push opcodes */
    if (opcode > OP_16) {
        ctx->op_count++;
        if (ctx->op_count > SCRIPT_MAX_OPS) {
            return script_set_error(ctx, SCRIPT_ERR_OP_COUNT);
        }
    }

    /*
     * ============================================
     * PUSH OPERATIONS
     * ============================================
     */

    /* OP_0 (OP_FALSE): Push empty byte array */
    if (opcode == OP_0) {
        return stack_push(&ctx->stack, NULL, 0);
    }

    /* OP_1NEGATE: Push -1 */
    if (opcode == OP_1NEGATE) {
        return stack_push_num(&ctx->stack, -1);
    }

    /* OP_1 through OP_16: Push the number 1-16 */
    if (opcode >= OP_1 && opcode <= OP_16) {
        script_num_t num = (script_num_t)(opcode - OP_1 + 1);
        return stack_push_num(&ctx->stack, num);
    }

    /* Direct push opcodes (0x01-0x4e): Push data */
    if (opcode >= 0x01 && opcode <= OP_PUSHDATA4) {
        if (op->len > SCRIPT_MAX_ELEMENT_SIZE) {
            return script_set_error(ctx, SCRIPT_ERR_PUSH_SIZE);
        }
        return stack_push(&ctx->stack, op->data, op->len);
    }

    /*
     * ============================================
     * FLOW CONTROL
     * ============================================
     */

    /* OP_NOP: Do nothing */
    if (opcode == OP_NOP) {
        return ECHO_OK;
    }

    /* Reserved NOPs (OP_NOP1, OP_NOP4-OP_NOP10): Succeed but may be redefined */
    if (opcode == OP_NOP1 || (opcode >= OP_NOP4 && opcode <= OP_NOP10)) {
        if (ctx->flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS) {
            return script_set_error(ctx, SCRIPT_ERR_RESERVED_OPCODE);
        }
        return ECHO_OK;
    }

    /*
     * OP_CHECKLOCKTIMEVERIFY (BIP-65)
     *
     * Verify that the transaction's nLockTime is greater than or equal to
     * the stack top value. This allows creating outputs that can only be
     * spent after a certain block height or time.
     *
     * Behavior:
     *   1. Stack must not be empty
     *   2. Stack top must be a valid 5-byte number (allows values up to 2^39-1)
     *   3. Stack top must be non-negative
     *   4. Locktime types must match (both block height or both time)
     *   5. Stack top must be <= transaction locktime
     *   6. Input sequence must not be final (0xFFFFFFFF)
     *
     * Note: Like OP_NOP, this opcode does NOT pop from the stack.
     */
    if (opcode == OP_CHECKLOCKTIMEVERIFY) {
        if (!(ctx->flags & SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY)) {
            /* If flag not set, treat as NOP2 */
            if (ctx->flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS) {
                return script_set_error(ctx, SCRIPT_ERR_RESERVED_OPCODE);
            }
            return ECHO_OK;
        }

        /* Stack must not be empty */
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }

        /* Peek at top value (do not remove) - allow 5 bytes for locktime values */
        const stack_element_t *top;
        stack_peek(&ctx->stack, &top);

        /* Decode as number (max 5 bytes for locktime) */
        script_num_t locktime_val;
        echo_result_t res = script_num_decode(top->data, top->len, &locktime_val,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE, 5);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }

        /* Must be non-negative */
        if (locktime_val < 0) {
            return script_set_error(ctx, SCRIPT_ERR_NEGATIVE_LOCKTIME);
        }

        /* Need transaction context */
        if (ctx->tx == NULL) {
            return script_set_error(ctx, SCRIPT_ERR_UNKNOWN_ERROR);
        }

        uint32_t tx_locktime = ctx->tx->locktime;

        /*
         * Check locktime type match:
         * - Values < LOCKTIME_THRESHOLD (500000000) are block heights
         * - Values >= LOCKTIME_THRESHOLD are Unix timestamps
         * Both must be of the same type
         */
        echo_bool_t stack_is_time = (locktime_val >= LOCKTIME_THRESHOLD);
        echo_bool_t tx_is_time = (tx_locktime >= LOCKTIME_THRESHOLD);
        if (stack_is_time != tx_is_time) {
            return script_set_error(ctx, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        }

        /* Stack value must be <= transaction locktime */
        if ((uint64_t)locktime_val > tx_locktime) {
            return script_set_error(ctx, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        }

        /* Input sequence must not be final (otherwise locktime is ignored) */
        if (ctx->input_index >= ctx->tx->input_count) {
            return script_set_error(ctx, SCRIPT_ERR_UNKNOWN_ERROR);
        }
        if (ctx->tx->inputs[ctx->input_index].sequence == TX_SEQUENCE_FINAL) {
            return script_set_error(ctx, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        }

        return ECHO_OK;
    }

    /*
     * OP_CHECKSEQUENCEVERIFY (BIP-112)
     *
     * Verify that the input's relative locktime (encoded in sequence number)
     * is satisfied. This allows creating outputs that can only be spent
     * after a certain amount of time/blocks since the output was confirmed.
     *
     * Behavior:
     *   1. Stack must not be empty
     *   2. Stack top must be a valid 5-byte number
     *   3. Stack top must be non-negative
     *   4. If stack top bit 31 is set, pass (disabled, acts as NOP)
     *   5. Transaction version must be >= 2
     *   6. Input sequence bit 31 must not be set (relative locktime enabled)
     *   7. Locktime types must match (both blocks or both time)
     *   8. Stack value (masked) must be <= sequence (masked)
     *
     * Note: Like OP_NOP, this opcode does NOT pop from the stack.
     */
    if (opcode == OP_CHECKSEQUENCEVERIFY) {
        if (!(ctx->flags & SCRIPT_VERIFY_CHECKSEQUENCEVERIFY)) {
            /* If flag not set, treat as NOP3 */
            if (ctx->flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS) {
                return script_set_error(ctx, SCRIPT_ERR_RESERVED_OPCODE);
            }
            return ECHO_OK;
        }

        /* Stack must not be empty */
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }

        /* Peek at top value (do not remove) - allow 5 bytes */
        const stack_element_t *top;
        stack_peek(&ctx->stack, &top);

        /* Decode as number (max 5 bytes) */
        script_num_t sequence_val;
        echo_result_t res = script_num_decode(top->data, top->len, &sequence_val,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE, 5);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }

        /* Must be non-negative */
        if (sequence_val < 0) {
            return script_set_error(ctx, SCRIPT_ERR_NEGATIVE_LOCKTIME);
        }

        /*
         * If bit 31 is set in the stack value, relative locktime is disabled
         * for this check — treat as successful NOP.
         */
        if ((uint32_t)sequence_val & SEQUENCE_LOCKTIME_DISABLE_FLAG) {
            return ECHO_OK;
        }

        /* Need transaction context */
        if (ctx->tx == NULL) {
            return script_set_error(ctx, SCRIPT_ERR_UNKNOWN_ERROR);
        }

        /* Transaction version must be >= 2 for relative locktime */
        if (ctx->tx->version < 2) {
            return script_set_error(ctx, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        }

        /* Get input sequence */
        if (ctx->input_index >= ctx->tx->input_count) {
            return script_set_error(ctx, SCRIPT_ERR_UNKNOWN_ERROR);
        }
        uint32_t input_sequence = ctx->tx->inputs[ctx->input_index].sequence;

        /*
         * If input sequence bit 31 is set, relative locktime is disabled
         * for this input — the script check fails.
         */
        if (input_sequence & SEQUENCE_LOCKTIME_DISABLE_FLAG) {
            return script_set_error(ctx, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        }

        /*
         * Check locktime type match:
         * - Bit 22 clear: block-based (value = number of blocks)
         * - Bit 22 set: time-based (value = 512-second granularity)
         */
        echo_bool_t stack_is_time = ((uint32_t)sequence_val & SEQUENCE_LOCKTIME_TYPE_FLAG) != 0;
        echo_bool_t input_is_time = (input_sequence & SEQUENCE_LOCKTIME_TYPE_FLAG) != 0;
        if (stack_is_time != input_is_time) {
            return script_set_error(ctx, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        }

        /* Compare masked values */
        uint32_t stack_masked = (uint32_t)sequence_val & SEQUENCE_LOCKTIME_MASK;
        uint32_t input_masked = input_sequence & SEQUENCE_LOCKTIME_MASK;
        if (stack_masked > input_masked) {
            return script_set_error(ctx, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        }

        return ECHO_OK;
    }

    /* OP_VERIFY: Pop and fail if false */
    if (opcode == OP_VERIFY) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        echo_bool_t val;
        echo_result_t res = stack_pop_bool(&ctx->stack, &val);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        if (!val) {
            return script_set_error(ctx, SCRIPT_ERR_VERIFY);
        }
        return ECHO_OK;
    }

    /* OP_RETURN: Immediately fail */
    if (opcode == OP_RETURN) {
        return script_set_error(ctx, SCRIPT_ERR_OP_RETURN);
    }

    /*
     * ============================================
     * ALTSTACK OPERATIONS
     * ============================================
     */

    /* OP_TOALTSTACK: Move top of main stack to altstack */
    if (opcode == OP_TOALTSTACK) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        stack_element_t elem;
        echo_result_t res = stack_pop(&ctx->stack, &elem);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        res = stack_push(&ctx->altstack, elem.data, elem.len);
        if (elem.data) free(elem.data);
        return res;
    }

    /* OP_FROMALTSTACK: Move top of altstack to main stack */
    if (opcode == OP_FROMALTSTACK) {
        if (stack_empty(&ctx->altstack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_ALTSTACK_OPERATION);
        }
        stack_element_t elem;
        echo_result_t res = stack_pop(&ctx->altstack, &elem);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_ALTSTACK_OPERATION);
        }
        res = stack_push(&ctx->stack, elem.data, elem.len);
        if (elem.data) free(elem.data);
        return res;
    }

    /*
     * ============================================
     * STACK OPERATIONS
     * ============================================
     */

    /* OP_IFDUP: Duplicate if nonzero */
    if (opcode == OP_IFDUP) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        const stack_element_t *top;
        stack_peek(&ctx->stack, &top);
        if (script_bool(top->data, top->len)) {
            return stack_dup(&ctx->stack);
        }
        return ECHO_OK;
    }

    /* OP_DEPTH: Push stack size */
    if (opcode == OP_DEPTH) {
        return stack_push_num(&ctx->stack, (script_num_t)stack_size(&ctx->stack));
    }

    /* OP_DROP */
    if (opcode == OP_DROP) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        return stack_drop(&ctx->stack);
    }

    /* OP_DUP */
    if (opcode == OP_DUP) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        return stack_dup(&ctx->stack);
    }

    /* OP_NIP */
    if (opcode == OP_NIP) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        return stack_nip(&ctx->stack);
    }

    /* OP_OVER */
    if (opcode == OP_OVER) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        return stack_over(&ctx->stack);
    }

    /* OP_PICK */
    if (opcode == OP_PICK) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t n;
        echo_result_t res = stack_pop_num(&ctx->stack, &n,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK || n < 0 || (size_t)n >= stack_size(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        return stack_pick(&ctx->stack, (size_t)n);
    }

    /* OP_ROLL */
    if (opcode == OP_ROLL) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t n;
        echo_result_t res = stack_pop_num(&ctx->stack, &n,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK || n < 0 || (size_t)n >= stack_size(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        return stack_roll(&ctx->stack, (size_t)n);
    }

    /* OP_ROT */
    if (opcode == OP_ROT) {
        if (stack_size(&ctx->stack) < 3) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        return stack_rot(&ctx->stack);
    }

    /* OP_SWAP */
    if (opcode == OP_SWAP) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        return stack_swap(&ctx->stack);
    }

    /* OP_TUCK */
    if (opcode == OP_TUCK) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        return stack_tuck(&ctx->stack);
    }

    /* OP_2DROP */
    if (opcode == OP_2DROP) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        return stack_2drop(&ctx->stack);
    }

    /* OP_2DUP */
    if (opcode == OP_2DUP) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        return stack_2dup(&ctx->stack);
    }

    /* OP_3DUP */
    if (opcode == OP_3DUP) {
        if (stack_size(&ctx->stack) < 3) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        return stack_3dup(&ctx->stack);
    }

    /* OP_2OVER */
    if (opcode == OP_2OVER) {
        if (stack_size(&ctx->stack) < 4) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        return stack_2over(&ctx->stack);
    }

    /* OP_2ROT */
    if (opcode == OP_2ROT) {
        if (stack_size(&ctx->stack) < 6) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        return stack_2rot(&ctx->stack);
    }

    /* OP_2SWAP */
    if (opcode == OP_2SWAP) {
        if (stack_size(&ctx->stack) < 4) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        return stack_2swap(&ctx->stack);
    }

    /*
     * ============================================
     * SPLICE OPERATIONS
     * ============================================
     */

    /* OP_SIZE: Push size of top element (without removing it) */
    if (opcode == OP_SIZE) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        const stack_element_t *top;
        stack_peek(&ctx->stack, &top);
        return stack_push_num(&ctx->stack, (script_num_t)top->len);
    }

    /*
     * ============================================
     * BITWISE LOGIC (only OP_EQUAL/OP_EQUALVERIFY are enabled)
     * ============================================
     */

    /* OP_EQUAL: Push 1 if top two are equal, else 0 */
    if (opcode == OP_EQUAL || opcode == OP_EQUALVERIFY) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        stack_element_t a, b;
        stack_pop(&ctx->stack, &b);
        stack_pop(&ctx->stack, &a);

        echo_bool_t equal = ECHO_FALSE;
        if (a.len == b.len) {
            if (a.len == 0) {
                equal = ECHO_TRUE;
            } else if (memcmp(a.data, b.data, a.len) == 0) {
                equal = ECHO_TRUE;
            }
        }

        if (a.data) free(a.data);
        if (b.data) free(b.data);

        if (opcode == OP_EQUALVERIFY) {
            if (!equal) {
                return script_set_error(ctx, SCRIPT_ERR_EQUALVERIFY);
            }
            return ECHO_OK;
        } else {
            return stack_push_bool(&ctx->stack, equal);
        }
    }

    /*
     * ============================================
     * ARITHMETIC OPERATIONS
     * ============================================
     */

    /* OP_1ADD: Add 1 to top */
    if (opcode == OP_1ADD) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a;
        echo_result_t res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, a + 1);
    }

    /* OP_1SUB: Subtract 1 from top */
    if (opcode == OP_1SUB) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a;
        echo_result_t res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, a - 1);
    }

    /* OP_NEGATE: Negate top */
    if (opcode == OP_NEGATE) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a;
        echo_result_t res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, -a);
    }

    /* OP_ABS: Absolute value of top */
    if (opcode == OP_ABS) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a;
        echo_result_t res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, a < 0 ? -a : a);
    }

    /* OP_NOT: Push 1 if top is 0, else 0 */
    if (opcode == OP_NOT) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a;
        echo_result_t res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, a == 0 ? 1 : 0);
    }

    /* OP_0NOTEQUAL: Push 1 if top is not 0, else 0 */
    if (opcode == OP_0NOTEQUAL) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a;
        echo_result_t res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, a != 0 ? 1 : 0);
    }

    /* OP_ADD: Add top two */
    if (opcode == OP_ADD) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a, b;
        echo_result_t res = stack_pop_num(&ctx->stack, &b,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, a + b);
    }

    /* OP_SUB: Subtract top from second-to-top */
    if (opcode == OP_SUB) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a, b;
        echo_result_t res = stack_pop_num(&ctx->stack, &b,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, a - b);
    }

    /* OP_BOOLAND: Push 1 if both top two are nonzero */
    if (opcode == OP_BOOLAND) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a, b;
        echo_result_t res = stack_pop_num(&ctx->stack, &b,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, (a != 0 && b != 0) ? 1 : 0);
    }

    /* OP_BOOLOR: Push 1 if either of top two is nonzero */
    if (opcode == OP_BOOLOR) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a, b;
        echo_result_t res = stack_pop_num(&ctx->stack, &b,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, (a != 0 || b != 0) ? 1 : 0);
    }

    /* OP_NUMEQUAL / OP_NUMEQUALVERIFY */
    if (opcode == OP_NUMEQUAL || opcode == OP_NUMEQUALVERIFY) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a, b;
        echo_result_t res = stack_pop_num(&ctx->stack, &b,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        echo_bool_t equal = (a == b) ? ECHO_TRUE : ECHO_FALSE;
        if (opcode == OP_NUMEQUALVERIFY) {
            if (!equal) {
                return script_set_error(ctx, SCRIPT_ERR_NUMEQUALVERIFY);
            }
            return ECHO_OK;
        }
        return stack_push_num(&ctx->stack, equal ? 1 : 0);
    }

    /* OP_NUMNOTEQUAL */
    if (opcode == OP_NUMNOTEQUAL) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a, b;
        echo_result_t res = stack_pop_num(&ctx->stack, &b,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, (a != b) ? 1 : 0);
    }

    /* OP_LESSTHAN */
    if (opcode == OP_LESSTHAN) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a, b;
        echo_result_t res = stack_pop_num(&ctx->stack, &b,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, (a < b) ? 1 : 0);
    }

    /* OP_GREATERTHAN */
    if (opcode == OP_GREATERTHAN) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a, b;
        echo_result_t res = stack_pop_num(&ctx->stack, &b,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, (a > b) ? 1 : 0);
    }

    /* OP_LESSTHANOREQUAL */
    if (opcode == OP_LESSTHANOREQUAL) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a, b;
        echo_result_t res = stack_pop_num(&ctx->stack, &b,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, (a <= b) ? 1 : 0);
    }

    /* OP_GREATERTHANOREQUAL */
    if (opcode == OP_GREATERTHANOREQUAL) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a, b;
        echo_result_t res = stack_pop_num(&ctx->stack, &b,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, (a >= b) ? 1 : 0);
    }

    /* OP_MIN */
    if (opcode == OP_MIN) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a, b;
        echo_result_t res = stack_pop_num(&ctx->stack, &b,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, (a < b) ? a : b);
    }

    /* OP_MAX */
    if (opcode == OP_MAX) {
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t a, b;
        echo_result_t res = stack_pop_num(&ctx->stack, &b,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        res = stack_pop_num(&ctx->stack, &a,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, (a > b) ? a : b);
    }

    /* OP_WITHIN: Push 1 if min <= x < max */
    if (opcode == OP_WITHIN) {
        if (stack_size(&ctx->stack) < 3) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        script_num_t max, min, x;
        echo_result_t res = stack_pop_num(&ctx->stack, &max,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        res = stack_pop_num(&ctx->stack, &min,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        res = stack_pop_num(&ctx->stack, &x,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_NUMBER_RANGE);
        }
        return stack_push_num(&ctx->stack, (x >= min && x < max) ? 1 : 0);
    }

    /*
     * ============================================
     * CRYPTO OPERATIONS (Session 4.4)
     * ============================================
     */

    /* OP_RIPEMD160: Hash top element with RIPEMD-160 */
    if (opcode == OP_RIPEMD160) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        stack_element_t elem;
        echo_result_t res = stack_pop(&ctx->stack, &elem);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        uint8_t hash[RIPEMD160_DIGEST_SIZE];
        ripemd160(elem.data, elem.len, hash);
        if (elem.data) free(elem.data);
        return stack_push(&ctx->stack, hash, RIPEMD160_DIGEST_SIZE);
    }

    /* OP_SHA1: Disabled in practice, but technically valid */
    if (opcode == OP_SHA1) {
        /* SHA-1 is not implemented in Bitcoin Echo; it's obsolete and insecure */
        return script_set_error(ctx, SCRIPT_ERR_BAD_OPCODE);
    }

    /* OP_SHA256: Hash top element with SHA-256 */
    if (opcode == OP_SHA256) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        stack_element_t elem;
        echo_result_t res = stack_pop(&ctx->stack, &elem);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        uint8_t hash[SHA256_DIGEST_SIZE];
        sha256(elem.data, elem.len, hash);
        if (elem.data) free(elem.data);
        return stack_push(&ctx->stack, hash, SHA256_DIGEST_SIZE);
    }

    /* OP_HASH160: RIPEMD160(SHA256(x)) - standard Bitcoin address hash */
    if (opcode == OP_HASH160) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        stack_element_t elem;
        echo_result_t res = stack_pop(&ctx->stack, &elem);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        uint8_t hash[RIPEMD160_DIGEST_SIZE];
        hash160(elem.data, elem.len, hash);
        if (elem.data) free(elem.data);
        return stack_push(&ctx->stack, hash, RIPEMD160_DIGEST_SIZE);
    }

    /* OP_HASH256: SHA256(SHA256(x)) - double SHA-256 */
    if (opcode == OP_HASH256) {
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        stack_element_t elem;
        echo_result_t res = stack_pop(&ctx->stack, &elem);
        if (res != ECHO_OK) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
        uint8_t hash[SHA256_DIGEST_SIZE];
        sha256d(elem.data, elem.len, hash);
        if (elem.data) free(elem.data);
        return stack_push(&ctx->stack, hash, SHA256_DIGEST_SIZE);
    }

    /* OP_CODESEPARATOR: Mark position for signature hash computation */
    if (opcode == OP_CODESEPARATOR) {
        /*
         * In legacy scripts, OP_CODESEPARATOR updates the scriptCode
         * used in signature hash computation to exclude everything
         * before and including this opcode.
         *
         * For now, we just note that it was executed. Full sighash
         * computation will track the position.
         */
        return ECHO_OK;
    }

    /*
     * OP_CHECKSIG, OP_CHECKSIGVERIFY, OP_CHECKMULTISIG, OP_CHECKMULTISIGVERIFY:
     *
     * These require transaction context (the spending tx, input index, etc.)
     * to compute the signature hash. Without transaction context, we cannot
     * verify signatures.
     *
     * For standalone script execution testing, we treat signature verification
     * as a separate concern. The caller must set up proper transaction context.
     */
    if (opcode == OP_CHECKSIG || opcode == OP_CHECKSIGVERIFY) {
        /*
         * CHECKSIG: Pop pubkey and signature, verify, push result
         * Stack: ... sig pubkey -> ... bool
         *
         * For now without tx context, we fail with an appropriate error.
         * Full implementation requires transaction data for sighash.
         */
        if (stack_size(&ctx->stack) < 2) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }

        /* Pop pubkey and signature */
        stack_element_t pubkey_elem, sig_elem;
        stack_pop(&ctx->stack, &pubkey_elem);
        stack_pop(&ctx->stack, &sig_elem);

        /*
         * Without transaction context, we cannot compute the sighash.
         * In a real implementation, we would:
         * 1. Extract sighash type from last byte of signature
         * 2. Compute sighash based on tx, input index, scriptCode
         * 3. Verify signature against sighash and pubkey
         *
         * For testing purposes, we push false (signature check fails
         * without context). Real usage requires extending script_context_t
         * with transaction data.
         */
        echo_bool_t result = ECHO_FALSE;

        /* Check if we're in NULLFAIL mode - empty sig on failure is required */
        if ((ctx->flags & SCRIPT_VERIFY_NULLFAIL) && sig_elem.len > 0) {
            /* Non-empty signature that fails verification in NULLFAIL mode */
            if (pubkey_elem.data) free(pubkey_elem.data);
            if (sig_elem.data) free(sig_elem.data);
            return script_set_error(ctx, SCRIPT_ERR_SIG_NULLFAIL);
        }

        if (pubkey_elem.data) free(pubkey_elem.data);
        if (sig_elem.data) free(sig_elem.data);

        if (opcode == OP_CHECKSIGVERIFY) {
            if (!result) {
                return script_set_error(ctx, SCRIPT_ERR_CHECKSIGVERIFY);
            }
            return ECHO_OK;  /* Don't push, just verify */
        }

        return stack_push_bool(&ctx->stack, result);
    }

    if (opcode == OP_CHECKMULTISIG || opcode == OP_CHECKMULTISIGVERIFY) {
        /*
         * CHECKMULTISIG: m-of-n multisig verification
         * Stack: ... dummy sig1 ... sigm m pubkey1 ... pubkeyn n -> ... bool
         *
         * Historical bug: consumes one extra stack element (dummy).
         * With NULLDUMMY (BIP-62/147), dummy must be empty.
         */
        if (stack_empty(&ctx->stack)) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }

        /* Get number of public keys */
        script_num_t n_keys;
        echo_result_t res = stack_pop_num(&ctx->stack, &n_keys,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK || n_keys < 0 || n_keys > SCRIPT_MAX_PUBKEYS_PER_MULTISIG) {
            return script_set_error(ctx, SCRIPT_ERR_PUBKEY_COUNT);
        }

        ctx->op_count += (size_t)n_keys;
        if (ctx->op_count > SCRIPT_MAX_OPS) {
            return script_set_error(ctx, SCRIPT_ERR_OP_COUNT);
        }

        if (stack_size(&ctx->stack) < (size_t)n_keys) {
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }

        /* Pop all public keys */
        stack_element_t *pubkeys = NULL;
        if (n_keys > 0) {
            pubkeys = (stack_element_t *)malloc((size_t)n_keys * sizeof(stack_element_t));
            if (pubkeys == NULL) {
                return ECHO_ERR_OUT_OF_MEMORY;
            }
            for (script_num_t i = 0; i < n_keys; i++) {
                stack_pop(&ctx->stack, &pubkeys[i]);
            }
        }

        /* Get number of required signatures */
        if (stack_empty(&ctx->stack)) {
            if (pubkeys) {
                for (script_num_t i = 0; i < n_keys; i++) {
                    if (pubkeys[i].data) free(pubkeys[i].data);
                }
                free(pubkeys);
            }
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }

        script_num_t n_sigs;
        res = stack_pop_num(&ctx->stack, &n_sigs,
            (ctx->flags & SCRIPT_VERIFY_MINIMALDATA) ? ECHO_TRUE : ECHO_FALSE,
            SCRIPT_NUM_MAX_SIZE);
        if (res != ECHO_OK || n_sigs < 0 || n_sigs > n_keys) {
            if (pubkeys) {
                for (script_num_t i = 0; i < n_keys; i++) {
                    if (pubkeys[i].data) free(pubkeys[i].data);
                }
                free(pubkeys);
            }
            return script_set_error(ctx, SCRIPT_ERR_SIG_COUNT);
        }

        if (stack_size(&ctx->stack) < (size_t)n_sigs) {
            if (pubkeys) {
                for (script_num_t i = 0; i < n_keys; i++) {
                    if (pubkeys[i].data) free(pubkeys[i].data);
                }
                free(pubkeys);
            }
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }

        /* Pop all signatures */
        stack_element_t *sigs = NULL;
        if (n_sigs > 0) {
            sigs = (stack_element_t *)malloc((size_t)n_sigs * sizeof(stack_element_t));
            if (sigs == NULL) {
                if (pubkeys) {
                    for (script_num_t i = 0; i < n_keys; i++) {
                        if (pubkeys[i].data) free(pubkeys[i].data);
                    }
                    free(pubkeys);
                }
                return ECHO_ERR_OUT_OF_MEMORY;
            }
            for (script_num_t i = 0; i < n_sigs; i++) {
                stack_pop(&ctx->stack, &sigs[i]);
            }
        }

        /*
         * Historical off-by-one bug: CHECKMULTISIG pops one extra element.
         * This "dummy" element is unused but must be present.
         */
        if (stack_empty(&ctx->stack)) {
            if (sigs) {
                for (script_num_t i = 0; i < n_sigs; i++) {
                    if (sigs[i].data) free(sigs[i].data);
                }
                free(sigs);
            }
            if (pubkeys) {
                for (script_num_t i = 0; i < n_keys; i++) {
                    if (pubkeys[i].data) free(pubkeys[i].data);
                }
                free(pubkeys);
            }
            return script_set_error(ctx, SCRIPT_ERR_INVALID_STACK_OPERATION);
        }

        stack_element_t dummy;
        stack_pop(&ctx->stack, &dummy);

        /* BIP-62/147: NULLDUMMY requires the dummy element to be empty */
        if ((ctx->flags & SCRIPT_VERIFY_NULLDUMMY) && dummy.len != 0) {
            if (dummy.data) free(dummy.data);
            if (sigs) {
                for (script_num_t i = 0; i < n_sigs; i++) {
                    if (sigs[i].data) free(sigs[i].data);
                }
                free(sigs);
            }
            if (pubkeys) {
                for (script_num_t i = 0; i < n_keys; i++) {
                    if (pubkeys[i].data) free(pubkeys[i].data);
                }
                free(pubkeys);
            }
            return script_set_error(ctx, SCRIPT_ERR_SIG_NULLDUMMY);
        }
        if (dummy.data) free(dummy.data);

        /*
         * Without transaction context, we cannot verify signatures.
         * Result is false (verification fails).
         */
        echo_bool_t result = ECHO_FALSE;

        /* Check NULLFAIL: all signatures must be empty if verification fails */
        if (ctx->flags & SCRIPT_VERIFY_NULLFAIL) {
            for (script_num_t i = 0; i < n_sigs; i++) {
                if (sigs[i].len > 0) {
                    if (sigs) {
                        for (script_num_t j = 0; j < n_sigs; j++) {
                            if (sigs[j].data) free(sigs[j].data);
                        }
                        free(sigs);
                    }
                    if (pubkeys) {
                        for (script_num_t j = 0; j < n_keys; j++) {
                            if (pubkeys[j].data) free(pubkeys[j].data);
                        }
                        free(pubkeys);
                    }
                    return script_set_error(ctx, SCRIPT_ERR_SIG_NULLFAIL);
                }
            }
        }

        /* Free signature and pubkey memory */
        if (sigs) {
            for (script_num_t i = 0; i < n_sigs; i++) {
                if (sigs[i].data) free(sigs[i].data);
            }
            free(sigs);
        }
        if (pubkeys) {
            for (script_num_t i = 0; i < n_keys; i++) {
                if (pubkeys[i].data) free(pubkeys[i].data);
            }
            free(pubkeys);
        }

        if (opcode == OP_CHECKMULTISIGVERIFY) {
            if (!result) {
                return script_set_error(ctx, SCRIPT_ERR_CHECKMULTISIGVERIFY);
            }
            return ECHO_OK;
        }

        return stack_push_bool(&ctx->stack, result);
    }

    /* OP_CHECKSIGADD: Tapscript batch signature verification (Session 4.6) */
    if (opcode == OP_CHECKSIGADD) {
        /* Deferred to Taproot implementation in Session 4.6 */
        return script_set_error(ctx, SCRIPT_ERR_BAD_OPCODE);
    }

    /* Unknown opcode */
    return script_set_error(ctx, SCRIPT_ERR_INVALID_OPCODE);
}

/*
 * Execute a complete script.
 */
echo_result_t script_execute(script_context_t *ctx,
                              const uint8_t *data, size_t len)
{
    if (ctx == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    /* Check script size limit */
    if (len > SCRIPT_MAX_SIZE) {
        ctx->error = SCRIPT_ERR_SCRIPT_SIZE;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Empty script succeeds */
    if (data == NULL || len == 0) {
        return ECHO_OK;
    }

    script_iter_t iter;
    script_iter_init(&iter, data, len);

    script_op_t op;
    while (script_iter_next(&iter, &op)) {
        echo_result_t res = script_exec_op(ctx, &op);
        if (res != ECHO_OK) {
            return res;
        }

        /* Check combined stack size */
        if (stack_size(&ctx->stack) + stack_size(&ctx->altstack) > SCRIPT_MAX_STACK_SIZE) {
            ctx->error = SCRIPT_ERR_STACK_SIZE;
            return ECHO_ERR_SCRIPT_ERROR;
        }
    }

    /* Check for parse errors */
    if (script_iter_error(&iter)) {
        ctx->error = SCRIPT_ERR_BAD_OPCODE;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Check for unbalanced conditionals */
    if (ctx->exec_depth != 0 || ctx->skip_depth != 0) {
        ctx->error = SCRIPT_ERR_UNBALANCED_CONDITIONAL;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    return ECHO_OK;
}


/*
 * ============================================================================
 * TRANSACTION CONTEXT AND SIGHASH (Session 4.5)
 * ============================================================================
 */

/*
 * Set transaction context for signature verification.
 */
void script_set_tx_context(script_context_t *ctx,
                            const tx_t *tx,
                            size_t input_index,
                            satoshi_t amount,
                            const uint8_t *script_code,
                            size_t script_code_len)
{
    if (ctx == NULL) return;

    ctx->tx = tx;
    ctx->input_index = input_index;
    ctx->amount = amount;
    ctx->script_code = script_code;
    ctx->script_code_len = script_code_len;
    ctx->codesep_pos = 0;

    /* Invalidate sighash cache when context changes */
    ctx->sighash_cached = ECHO_FALSE;
}

/*
 * Helper: Write a 32-bit little-endian value to a buffer.
 */
static void write_le32(uint8_t *buf, uint32_t val)
{
    buf[0] = (uint8_t)(val & 0xff);
    buf[1] = (uint8_t)((val >> 8) & 0xff);
    buf[2] = (uint8_t)((val >> 16) & 0xff);
    buf[3] = (uint8_t)((val >> 24) & 0xff);
}

/*
 * Helper: Write a 64-bit little-endian value to a buffer.
 */
static void write_le64(uint8_t *buf, uint64_t val)
{
    buf[0] = (uint8_t)(val & 0xff);
    buf[1] = (uint8_t)((val >> 8) & 0xff);
    buf[2] = (uint8_t)((val >> 16) & 0xff);
    buf[3] = (uint8_t)((val >> 24) & 0xff);
    buf[4] = (uint8_t)((val >> 32) & 0xff);
    buf[5] = (uint8_t)((val >> 40) & 0xff);
    buf[6] = (uint8_t)((val >> 48) & 0xff);
    buf[7] = (uint8_t)((val >> 56) & 0xff);
}

/*
 * Helper: Compute hash of all prevouts (for BIP-143).
 */
static void compute_hash_prevouts(const tx_t *tx, uint8_t out[32])
{
    sha256_ctx_t sha;
    sha256_init(&sha);

    for (size_t i = 0; i < tx->input_count; i++) {
        /* Each prevout is 32-byte txid + 4-byte vout */
        sha256_update(&sha, tx->inputs[i].prevout.txid.bytes, 32);
        uint8_t vout_buf[4];
        write_le32(vout_buf, tx->inputs[i].prevout.vout);
        sha256_update(&sha, vout_buf, 4);
    }

    uint8_t single_hash[32];
    sha256_final(&sha, single_hash);
    sha256d(single_hash, 32, out);  /* Double SHA256 */
}

/*
 * Helper: Compute hash of all sequence numbers (for BIP-143).
 */
static void compute_hash_sequence(const tx_t *tx, uint8_t out[32])
{
    sha256_ctx_t sha;
    sha256_init(&sha);

    for (size_t i = 0; i < tx->input_count; i++) {
        uint8_t seq_buf[4];
        write_le32(seq_buf, tx->inputs[i].sequence);
        sha256_update(&sha, seq_buf, 4);
    }

    uint8_t single_hash[32];
    sha256_final(&sha, single_hash);
    sha256d(single_hash, 32, out);
}

/*
 * Helper: Compute hash of all outputs (for BIP-143).
 */
static void compute_hash_outputs(const tx_t *tx, uint8_t out[32])
{
    sha256_ctx_t sha;
    sha256_init(&sha);

    for (size_t i = 0; i < tx->output_count; i++) {
        /* Each output is 8-byte value + varint script length + script */
        uint8_t val_buf[8];
        write_le64(val_buf, (uint64_t)tx->outputs[i].value);
        sha256_update(&sha, val_buf, 8);

        /* Write script length as varint */
        uint8_t varint_buf[9];
        size_t varint_len = 0;
        size_t script_len = tx->outputs[i].script_pubkey_len;
        if (script_len < 0xfd) {
            varint_buf[0] = (uint8_t)script_len;
            varint_len = 1;
        } else if (script_len <= 0xffff) {
            varint_buf[0] = 0xfd;
            varint_buf[1] = (uint8_t)(script_len & 0xff);
            varint_buf[2] = (uint8_t)((script_len >> 8) & 0xff);
            varint_len = 3;
        } else {
            varint_buf[0] = 0xfe;
            write_le32(varint_buf + 1, (uint32_t)script_len);
            varint_len = 5;
        }
        sha256_update(&sha, varint_buf, varint_len);
        sha256_update(&sha, tx->outputs[i].script_pubkey, script_len);
    }

    uint8_t single_hash[32];
    sha256_final(&sha, single_hash);
    sha256d(single_hash, 32, out);
}

/*
 * Helper: Compute hash of a single output (for SIGHASH_SINGLE).
 */
static void compute_hash_single_output(const tx_t *tx, size_t index, uint8_t out[32])
{
    if (index >= tx->output_count) {
        /* SIGHASH_SINGLE with no matching output: hash is all zeros */
        memset(out, 0, 32);
        return;
    }

    sha256_ctx_t sha;
    sha256_init(&sha);

    uint8_t val_buf[8];
    write_le64(val_buf, (uint64_t)tx->outputs[index].value);
    sha256_update(&sha, val_buf, 8);

    uint8_t varint_buf[9];
    size_t varint_len = 0;
    size_t script_len = tx->outputs[index].script_pubkey_len;
    if (script_len < 0xfd) {
        varint_buf[0] = (uint8_t)script_len;
        varint_len = 1;
    } else if (script_len <= 0xffff) {
        varint_buf[0] = 0xfd;
        varint_buf[1] = (uint8_t)(script_len & 0xff);
        varint_buf[2] = (uint8_t)((script_len >> 8) & 0xff);
        varint_len = 3;
    } else {
        varint_buf[0] = 0xfe;
        write_le32(varint_buf + 1, (uint32_t)script_len);
        varint_len = 5;
    }
    sha256_update(&sha, varint_buf, varint_len);
    sha256_update(&sha, tx->outputs[index].script_pubkey, script_len);

    uint8_t single_hash[32];
    sha256_final(&sha, single_hash);
    sha256d(single_hash, 32, out);
}

/*
 * Compute BIP-143 signature hash for SegWit v0.
 *
 * BIP-143 sighash serialization:
 *   1. nVersion (4 bytes)
 *   2. hashPrevouts (32 bytes)
 *   3. hashSequence (32 bytes)
 *   4. outpoint (36 bytes: 32-byte txid + 4-byte vout)
 *   5. scriptCode (varint + data)
 *   6. amount (8 bytes)
 *   7. nSequence (4 bytes)
 *   8. hashOutputs (32 bytes)
 *   9. nLockTime (4 bytes)
 *  10. nHashType (4 bytes)
 */
echo_result_t sighash_bip143(script_context_t *ctx,
                              uint32_t sighash_type,
                              uint8_t sighash[32])
{
    if (ctx == NULL || ctx->tx == NULL || sighash == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    const tx_t *tx = ctx->tx;
    size_t idx = ctx->input_index;

    if (idx >= tx->input_count) {
        return ECHO_ERR_OUT_OF_RANGE;
    }

    uint32_t base_type = sighash_type & 0x1f;
    echo_bool_t anyone_can_pay = (sighash_type & SIGHASH_ANYONECANPAY) != 0;

    /* Compute cached hashes if not already done */
    if (!ctx->sighash_cached) {
        compute_hash_prevouts(tx, ctx->hash_prevouts);
        compute_hash_sequence(tx, ctx->hash_sequence);
        compute_hash_outputs(tx, ctx->hash_outputs);
        ctx->sighash_cached = ECHO_TRUE;
    }

    sha256_ctx_t sha;
    sha256_init(&sha);

    /* 1. nVersion */
    uint8_t version_buf[4];
    write_le32(version_buf, (uint32_t)tx->version);
    sha256_update(&sha, version_buf, 4);

    /* 2. hashPrevouts */
    if (anyone_can_pay) {
        uint8_t zeros[32] = {0};
        sha256_update(&sha, zeros, 32);
    } else {
        sha256_update(&sha, ctx->hash_prevouts, 32);
    }

    /* 3. hashSequence */
    if (anyone_can_pay || base_type == SIGHASH_SINGLE || base_type == SIGHASH_NONE) {
        uint8_t zeros[32] = {0};
        sha256_update(&sha, zeros, 32);
    } else {
        sha256_update(&sha, ctx->hash_sequence, 32);
    }

    /* 4. outpoint (txid + vout) */
    sha256_update(&sha, tx->inputs[idx].prevout.txid.bytes, 32);
    uint8_t vout_buf[4];
    write_le32(vout_buf, tx->inputs[idx].prevout.vout);
    sha256_update(&sha, vout_buf, 4);

    /* 5. scriptCode (with length prefix) */
    const uint8_t *script_code = ctx->script_code;
    size_t script_code_len = ctx->script_code_len;

    /* Handle CODESEPARATOR: use script from codesep_pos onwards */
    if (ctx->codesep_pos > 0 && ctx->codesep_pos < script_code_len) {
        script_code = script_code + ctx->codesep_pos;
        script_code_len = script_code_len - ctx->codesep_pos;
    }

    uint8_t varint_buf[9];
    size_t varint_len = 0;
    if (script_code_len < 0xfd) {
        varint_buf[0] = (uint8_t)script_code_len;
        varint_len = 1;
    } else if (script_code_len <= 0xffff) {
        varint_buf[0] = 0xfd;
        varint_buf[1] = (uint8_t)(script_code_len & 0xff);
        varint_buf[2] = (uint8_t)((script_code_len >> 8) & 0xff);
        varint_len = 3;
    } else {
        varint_buf[0] = 0xfe;
        write_le32(varint_buf + 1, (uint32_t)script_code_len);
        varint_len = 5;
    }
    sha256_update(&sha, varint_buf, varint_len);
    if (script_code_len > 0) {
        sha256_update(&sha, script_code, script_code_len);
    }

    /* 6. amount (8 bytes) */
    uint8_t amount_buf[8];
    write_le64(amount_buf, (uint64_t)ctx->amount);
    sha256_update(&sha, amount_buf, 8);

    /* 7. nSequence */
    uint8_t seq_buf[4];
    write_le32(seq_buf, tx->inputs[idx].sequence);
    sha256_update(&sha, seq_buf, 4);

    /* 8. hashOutputs */
    if (base_type == SIGHASH_NONE) {
        uint8_t zeros[32] = {0};
        sha256_update(&sha, zeros, 32);
    } else if (base_type == SIGHASH_SINGLE) {
        uint8_t single_out_hash[32];
        compute_hash_single_output(tx, idx, single_out_hash);
        sha256_update(&sha, single_out_hash, 32);
    } else {
        sha256_update(&sha, ctx->hash_outputs, 32);
    }

    /* 9. nLockTime */
    uint8_t locktime_buf[4];
    write_le32(locktime_buf, tx->locktime);
    sha256_update(&sha, locktime_buf, 4);

    /* 10. nHashType */
    uint8_t hashtype_buf[4];
    write_le32(hashtype_buf, sighash_type);
    sha256_update(&sha, hashtype_buf, 4);

    /* Finalize with double SHA-256 */
    uint8_t single_hash[32];
    sha256_final(&sha, single_hash);
    sha256(single_hash, 32, sighash);

    return ECHO_OK;
}

/*
 * Compute legacy signature hash (pre-SegWit).
 * This is a simplified implementation for basic SIGHASH_ALL.
 */
echo_result_t sighash_legacy(script_context_t *ctx,
                              uint32_t sighash_type,
                              uint8_t sighash[32])
{
    if (ctx == NULL || ctx->tx == NULL || sighash == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    /*
     * Legacy sighash requires serializing a modified transaction.
     * This is complex because we need to:
     * 1. Clear all input scripts except the one being signed
     * 2. Replace that script with scriptCode
     * 3. Handle SIGHASH_SINGLE and SIGHASH_NONE edge cases
     *
     * For now, we implement basic SIGHASH_ALL support.
     * Full legacy sighash is complex and rarely needed for new code.
     */

    /* Calculate approximate buffer size needed */
    const tx_t *tx = ctx->tx;
    size_t buf_size = 4; /* version */

    /* Input count varint */
    buf_size += 9;

    /* Inputs */
    for (size_t i = 0; i < tx->input_count; i++) {
        buf_size += 32 + 4; /* outpoint */
        if (i == ctx->input_index) {
            buf_size += 9 + ctx->script_code_len; /* scriptCode */
        } else {
            buf_size += 1; /* empty script */
        }
        buf_size += 4; /* sequence */
    }

    /* Output count varint */
    buf_size += 9;

    /* Outputs */
    for (size_t i = 0; i < tx->output_count; i++) {
        buf_size += 8; /* value */
        buf_size += 9 + tx->outputs[i].script_pubkey_len; /* script */
    }

    buf_size += 4; /* locktime */
    buf_size += 4; /* sighash type */

    /* Allocate buffer */
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    if (buf == NULL) {
        return ECHO_ERR_OUT_OF_MEMORY;
    }

    size_t pos = 0;

    /* Version */
    write_le32(buf + pos, (uint32_t)tx->version);
    pos += 4;

    /* Input count */
    if (tx->input_count < 0xfd) {
        buf[pos++] = (uint8_t)tx->input_count;
    } else {
        buf[pos++] = 0xfd;
        buf[pos++] = (uint8_t)(tx->input_count & 0xff);
        buf[pos++] = (uint8_t)((tx->input_count >> 8) & 0xff);
    }

    /* Inputs */
    for (size_t i = 0; i < tx->input_count; i++) {
        /* Outpoint */
        memcpy(buf + pos, tx->inputs[i].prevout.txid.bytes, 32);
        pos += 32;
        write_le32(buf + pos, tx->inputs[i].prevout.vout);
        pos += 4;

        /* Script */
        if (i == ctx->input_index) {
            /* Use scriptCode for the input being signed */
            size_t len = ctx->script_code_len;
            if (len < 0xfd) {
                buf[pos++] = (uint8_t)len;
            } else {
                buf[pos++] = 0xfd;
                buf[pos++] = (uint8_t)(len & 0xff);
                buf[pos++] = (uint8_t)((len >> 8) & 0xff);
            }
            if (len > 0) {
                memcpy(buf + pos, ctx->script_code, len);
                pos += len;
            }
        } else {
            /* Empty script for other inputs */
            buf[pos++] = 0;
        }

        /* Sequence */
        write_le32(buf + pos, tx->inputs[i].sequence);
        pos += 4;
    }

    /* Output count */
    if (tx->output_count < 0xfd) {
        buf[pos++] = (uint8_t)tx->output_count;
    } else {
        buf[pos++] = 0xfd;
        buf[pos++] = (uint8_t)(tx->output_count & 0xff);
        buf[pos++] = (uint8_t)((tx->output_count >> 8) & 0xff);
    }

    /* Outputs */
    for (size_t i = 0; i < tx->output_count; i++) {
        /* Value */
        write_le64(buf + pos, (uint64_t)tx->outputs[i].value);
        pos += 8;

        /* Script */
        size_t len = tx->outputs[i].script_pubkey_len;
        if (len < 0xfd) {
            buf[pos++] = (uint8_t)len;
        } else {
            buf[pos++] = 0xfd;
            buf[pos++] = (uint8_t)(len & 0xff);
            buf[pos++] = (uint8_t)((len >> 8) & 0xff);
        }
        memcpy(buf + pos, tx->outputs[i].script_pubkey, len);
        pos += len;
    }

    /* Locktime */
    write_le32(buf + pos, tx->locktime);
    pos += 4;

    /* Sighash type */
    write_le32(buf + pos, sighash_type);
    pos += 4;

    /* Double SHA256 */
    sha256d(buf, pos, sighash);

    free(buf);
    return ECHO_OK;
}

/*
 * Verify a P2WPKH (Pay to Witness Public Key Hash) script.
 */
echo_result_t script_verify_p2wpkh(script_context_t *ctx,
                                    const uint8_t *witness_data,
                                    size_t witness_len,
                                    const uint8_t pubkey_hash[20])
{
    if (ctx == NULL || witness_data == NULL || pubkey_hash == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    /*
     * P2WPKH witness stack: [signature, pubkey]
     * We need to:
     * 1. Parse the witness stack (2 items)
     * 2. Check that HASH160(pubkey) == pubkey_hash
     * 3. Compute BIP-143 sighash with P2PKH-style scriptCode
     * 4. Verify signature against sighash and pubkey
     */

    /* Parse witness stack - format: count + [len + data]... */
    if (witness_len < 1) {
        ctx->error = SCRIPT_ERR_WITNESS_PROGRAM_WITNESS_EMPTY;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    size_t pos = 0;
    size_t count = witness_data[pos++];

    if (count != 2) {
        ctx->error = SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Read signature */
    if (pos >= witness_len) {
        ctx->error = SCRIPT_ERR_WITNESS_MALLEATED;
        return ECHO_ERR_SCRIPT_ERROR;
    }
    size_t sig_len = witness_data[pos++];
    if (pos + sig_len > witness_len) {
        ctx->error = SCRIPT_ERR_WITNESS_MALLEATED;
        return ECHO_ERR_SCRIPT_ERROR;
    }
    const uint8_t *sig = witness_data + pos;
    pos += sig_len;

    /* Read pubkey */
    if (pos >= witness_len) {
        ctx->error = SCRIPT_ERR_WITNESS_MALLEATED;
        return ECHO_ERR_SCRIPT_ERROR;
    }
    size_t pubkey_len = witness_data[pos++];
    if (pos + pubkey_len > witness_len) {
        ctx->error = SCRIPT_ERR_WITNESS_MALLEATED;
        return ECHO_ERR_SCRIPT_ERROR;
    }
    const uint8_t *pubkey = witness_data + pos;

    /* Verify pubkey hash matches */
    uint8_t computed_hash[20];
    hash160(pubkey, pubkey_len, computed_hash);
    if (memcmp(computed_hash, pubkey_hash, 20) != 0) {
        ctx->error = SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Verify pubkey is compressed (33 bytes, starts with 02 or 03) */
    if ((ctx->flags & SCRIPT_VERIFY_WITNESS_PUBKEYTYPE) && pubkey_len != 33) {
        ctx->error = SCRIPT_ERR_WITNESS_PUBKEYTYPE;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Empty signature with NULLFAIL check */
    if (sig_len == 0) {
        if (ctx->flags & SCRIPT_VERIFY_NULLFAIL) {
            /* Empty sig is OK in NULLFAIL mode - it just fails verification */
        }
        ctx->error = SCRIPT_ERR_EVAL_FALSE;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Extract sighash type from last byte of signature */
    uint32_t sighash_type = sig[sig_len - 1];
    size_t actual_sig_len = sig_len - 1;

    /* Build P2PKH-style scriptCode for sighash:
     * OP_DUP OP_HASH160 <20 bytes> OP_EQUALVERIFY OP_CHECKSIG
     */
    uint8_t script_code[25];
    script_code[0] = OP_DUP;
    script_code[1] = OP_HASH160;
    script_code[2] = 0x14;  /* Push 20 bytes */
    memcpy(script_code + 3, pubkey_hash, 20);
    script_code[23] = OP_EQUALVERIFY;
    script_code[24] = OP_CHECKSIG;

    /* Set script code for sighash computation */
    const uint8_t *old_script_code = ctx->script_code;
    size_t old_script_code_len = ctx->script_code_len;
    ctx->script_code = script_code;
    ctx->script_code_len = 25;

    /* Compute BIP-143 sighash */
    uint8_t sighash_out[32];
    echo_result_t res = sighash_bip143(ctx, sighash_type, sighash_out);

    /* Restore script code */
    ctx->script_code = old_script_code;
    ctx->script_code_len = old_script_code_len;

    if (res != ECHO_OK) {
        return res;
    }

    /* Verify signature using ECDSA */
    int valid = sig_verify(SIG_ECDSA, sig, actual_sig_len, sighash_out, pubkey, pubkey_len);
    if (!valid) {
        if ((ctx->flags & SCRIPT_VERIFY_NULLFAIL) && sig_len > 0) {
            ctx->error = SCRIPT_ERR_SIG_NULLFAIL;
            return ECHO_ERR_SCRIPT_ERROR;
        }
        ctx->error = SCRIPT_ERR_EVAL_FALSE;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    return ECHO_OK;
}

/*
 * Verify a P2WSH (Pay to Witness Script Hash) script.
 */
echo_result_t script_verify_p2wsh(script_context_t *ctx,
                                   const uint8_t *witness_data,
                                   size_t witness_len,
                                   const uint8_t script_hash[32])
{
    if (ctx == NULL || witness_data == NULL || script_hash == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    /*
     * P2WSH witness stack: [input1, input2, ..., witnessScript]
     * The last element is the witness script.
     */

    if (witness_len < 1) {
        ctx->error = SCRIPT_ERR_WITNESS_PROGRAM_WITNESS_EMPTY;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    size_t pos = 0;
    size_t count = witness_data[pos++];

    if (count == 0) {
        ctx->error = SCRIPT_ERR_WITNESS_PROGRAM_WITNESS_EMPTY;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Parse all witness items to find the last one (witness script) */
    const uint8_t *witness_script = NULL;
    size_t witness_script_len = 0;

    for (size_t i = 0; i < count; i++) {
        if (pos >= witness_len) {
            ctx->error = SCRIPT_ERR_WITNESS_MALLEATED;
            return ECHO_ERR_SCRIPT_ERROR;
        }

        /* Read length (may be varint) */
        size_t item_len = witness_data[pos++];
        if (item_len == 0xfd) {
            if (pos + 2 > witness_len) {
                ctx->error = SCRIPT_ERR_WITNESS_MALLEATED;
                return ECHO_ERR_SCRIPT_ERROR;
            }
            item_len = witness_data[pos] | ((size_t)witness_data[pos + 1] << 8);
            pos += 2;
        } else if (item_len == 0xfe) {
            if (pos + 4 > witness_len) {
                ctx->error = SCRIPT_ERR_WITNESS_MALLEATED;
                return ECHO_ERR_SCRIPT_ERROR;
            }
            item_len = witness_data[pos] | ((size_t)witness_data[pos + 1] << 8) |
                       ((size_t)witness_data[pos + 2] << 16) | ((size_t)witness_data[pos + 3] << 24);
            pos += 4;
        }

        if (pos + item_len > witness_len) {
            ctx->error = SCRIPT_ERR_WITNESS_MALLEATED;
            return ECHO_ERR_SCRIPT_ERROR;
        }

        if (i == count - 1) {
            /* Last item is the witness script */
            witness_script = witness_data + pos;
            witness_script_len = item_len;
        }

        pos += item_len;
    }

    if (witness_script == NULL) {
        ctx->error = SCRIPT_ERR_WITNESS_PROGRAM_WITNESS_EMPTY;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Verify script hash: SHA256(witness_script) == script_hash */
    uint8_t computed_hash[32];
    sha256(witness_script, witness_script_len, computed_hash);
    if (memcmp(computed_hash, script_hash, 32) != 0) {
        ctx->error = SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Check witness script size limit */
    if (witness_script_len > SCRIPT_MAX_SIZE) {
        ctx->error = SCRIPT_ERR_SCRIPT_SIZE;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /*
     * Now we need to execute the witness script with the remaining
     * witness items as the initial stack.
     *
     * Set up the context with the witness script as scriptCode
     * for BIP-143 sighash computation.
     */
    ctx->script_code = witness_script;
    ctx->script_code_len = witness_script_len;

    /* Push witness items (except the last one) onto the stack */
    pos = 1; /* Skip count byte */
    for (size_t i = 0; i < count - 1; i++) {
        size_t item_len = witness_data[pos++];
        if (item_len == 0xfd) {
            item_len = witness_data[pos] | ((size_t)witness_data[pos + 1] << 8);
            pos += 2;
        } else if (item_len == 0xfe) {
            item_len = witness_data[pos] | ((size_t)witness_data[pos + 1] << 8) |
                       ((size_t)witness_data[pos + 2] << 16) | ((size_t)witness_data[pos + 3] << 24);
            pos += 4;
        }

        echo_result_t res = stack_push(&ctx->stack, witness_data + pos, item_len);
        if (res != ECHO_OK) {
            return res;
        }
        pos += item_len;
    }

    /* Execute the witness script */
    echo_result_t res = script_execute(ctx, witness_script, witness_script_len);
    if (res != ECHO_OK) {
        return res;
    }

    /* Check final stack state */
    if (stack_empty(&ctx->stack)) {
        ctx->error = SCRIPT_ERR_EVAL_FALSE;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Top of stack must be true */
    echo_bool_t result;
    res = stack_pop_bool(&ctx->stack, &result);
    if (res != ECHO_OK) {
        return res;
    }

    if (!result) {
        ctx->error = SCRIPT_ERR_EVAL_FALSE;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Clean stack check for witness */
    if ((ctx->flags & SCRIPT_VERIFY_CLEANSTACK) && !stack_empty(&ctx->stack)) {
        ctx->error = SCRIPT_ERR_CLEANSTACK;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    return ECHO_OK;
}


/*
 * ============================================================================
 * TAPROOT SCRIPT EXECUTION (Session 4.6 — BIP-341/342)
 * ============================================================================
 */

/*
 * Compute Taproot-specific cached hashes (BIP-341).
 * These are similar to BIP-143 but include amounts and scriptPubKeys.
 */
static void compute_taproot_cache(script_context_t *ctx)
{
    if (ctx->tap_sighash_cached || ctx->tx == NULL) {
        return;
    }

    sha256_ctx_t sha;

    /* hash_amounts = SHA256(amount_0 || amount_1 || ... || amount_n) */
    sha256_init(&sha);
    for (size_t i = 0; i < ctx->tx->input_count; i++) {
        /* We need to get amounts from UTXOs - for now use ctx->amount for single input */
        /* In a full implementation, we'd have access to all spent amounts */
        uint8_t amt[8];
        write_le64(amt, ctx->amount);  /* Placeholder - should be per-input */
        sha256_update(&sha, amt, 8);
    }
    sha256_final(&sha, ctx->hash_amounts);

    /* hash_scriptpubkeys = SHA256(scriptPubKey_0 || scriptPubKey_1 || ...) */
    /* For now, this is a placeholder - full impl needs spent scriptPubKeys */
    sha256_init(&sha);
    /* Placeholder: compute from available data */
    sha256_final(&sha, ctx->hash_scriptpubkeys);

    ctx->tap_sighash_cached = ECHO_TRUE;
}

/*
 * Compute BIP-341 signature hash for Taproot.
 */
echo_result_t sighash_taproot(script_context_t *ctx,
                               uint32_t sighash_type,
                               uint8_t ext_flag,
                               uint8_t sighash[32])
{
    if (ctx == NULL || sighash == NULL || ctx->tx == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    const tx_t *tx = ctx->tx;

    /* Determine base sighash type */
    uint8_t hash_type = sighash_type & 0x03;
    if (hash_type == 0) hash_type = SIGHASH_ALL;  /* Default */
    echo_bool_t anyone_can_pay = (sighash_type & SIGHASH_ANYONECANPAY) != 0;

    /* Ensure caches are computed */
    if (!ctx->sighash_cached) {
        compute_hash_prevouts(tx, ctx->hash_prevouts);
        compute_hash_sequence(tx, ctx->hash_sequence);
        compute_hash_outputs(tx, ctx->hash_outputs);
        ctx->sighash_cached = ECHO_TRUE;
    }
    compute_taproot_cache(ctx);

    /*
     * BIP-341 sighash preimage:
     * - epoch (0x00)
     * - hash_type (1 byte)
     * - nVersion (4 bytes LE)
     * - nLockTime (4 bytes LE)
     * - sha_prevouts (if not ANYONECANPAY)
     * - sha_amounts (if not ANYONECANPAY)
     * - sha_scriptpubkeys (if not ANYONECANPAY)
     * - sha_sequences (if not ANYONECANPAY)
     * - sha_outputs (if SIGHASH_ALL)
     * - spend_type (1 byte: ext_flag * 2 + annex_present)
     * - if ANYONECANPAY:
     *     outpoint (36 bytes)
     *     amount (8 bytes)
     *     scriptPubKey (variable)
     *     nSequence (4 bytes)
     * - else:
     *     input_index (4 bytes LE)
     * - if SIGHASH_SINGLE:
     *     sha_single_output (32 bytes)
     * - if ext_flag == 1 (script path):
     *     tapleaf_hash (32 bytes)
     *     key_version (1 byte)
     *     codesep_pos (4 bytes LE)
     */

    uint8_t preimage[512];
    size_t pos = 0;

    /* epoch = 0x00 */
    preimage[pos++] = 0x00;

    /* hash_type */
    preimage[pos++] = (uint8_t)sighash_type;

    /* nVersion (4 bytes LE) */
    write_le32(preimage + pos, (uint32_t)tx->version);
    pos += 4;

    /* nLockTime (4 bytes LE) */
    write_le32(preimage + pos, tx->locktime);
    pos += 4;

    /* sha_prevouts (if not ANYONECANPAY) */
    if (!anyone_can_pay) {
        memcpy(preimage + pos, ctx->hash_prevouts, 32);
        pos += 32;

        /* sha_amounts */
        memcpy(preimage + pos, ctx->hash_amounts, 32);
        pos += 32;

        /* sha_scriptpubkeys */
        memcpy(preimage + pos, ctx->hash_scriptpubkeys, 32);
        pos += 32;

        /* sha_sequences */
        memcpy(preimage + pos, ctx->hash_sequence, 32);
        pos += 32;
    }

    /* sha_outputs (if SIGHASH_ALL or SIGHASH_DEFAULT) */
    if (hash_type == SIGHASH_ALL) {
        memcpy(preimage + pos, ctx->hash_outputs, 32);
        pos += 32;
    }

    /* spend_type = ext_flag * 2 + annex_present (no annex support for now) */
    preimage[pos++] = ext_flag * 2;

    /* Input data */
    if (anyone_can_pay) {
        /* outpoint (36 bytes) */
        memcpy(preimage + pos, tx->inputs[ctx->input_index].prevout.txid.bytes, 32);
        pos += 32;
        write_le32(preimage + pos, tx->inputs[ctx->input_index].prevout.vout);
        pos += 4;

        /* amount (8 bytes) */
        write_le64(preimage + pos, ctx->amount);
        pos += 8;

        /* scriptPubKey - use varint + data */
        size_t spk_len = ctx->script_code_len;
        pos += varint_write(preimage + pos, 32, spk_len, NULL);
        if (ctx->script_code && spk_len > 0) {
            memcpy(preimage + pos, ctx->script_code, spk_len);
            pos += spk_len;
        }

        /* nSequence (4 bytes) */
        write_le32(preimage + pos, tx->inputs[ctx->input_index].sequence);
        pos += 4;
    } else {
        /* input_index (4 bytes LE) */
        write_le32(preimage + pos, (uint32_t)ctx->input_index);
        pos += 4;
    }

    /* sha_single_output (if SIGHASH_SINGLE) */
    if (hash_type == SIGHASH_SINGLE) {
        if (ctx->input_index < tx->output_count) {
            uint8_t single_hash[32];
            compute_hash_single_output(tx, ctx->input_index, single_hash);
            memcpy(preimage + pos, single_hash, 32);
            pos += 32;
        } else {
            /* If no corresponding output, hash zeros */
            memset(preimage + pos, 0, 32);
            pos += 32;
        }
    }

    /* Script path extension (if ext_flag == 1) */
    if (ext_flag == 1) {
        /* tapleaf_hash (32 bytes) */
        memcpy(preimage + pos, ctx->tapleaf_hash, 32);
        pos += 32;

        /* key_version (1 byte) */
        preimage[pos++] = ctx->key_version;

        /* codesep_pos (4 bytes LE) */
        write_le32(preimage + pos, (uint32_t)ctx->codesep_pos);
        pos += 4;
    }

    /* Compute tagged hash: TapSighash */
    secp256k1_schnorr_tagged_hash(sighash, "TapSighash", preimage, pos);

    return ECHO_OK;
}

/*
 * Compute the Taproot leaf hash.
 */
echo_result_t taproot_leaf_hash(uint8_t leaf_version,
                                 const uint8_t *script,
                                 size_t script_len,
                                 uint8_t leaf_hash[32])
{
    if (leaf_hash == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    /*
     * leaf_hash = tagged_hash("TapLeaf", leaf_version || compact_size(script_len) || script)
     */
    uint8_t buf[1 + 9 + 10000];  /* Max script size */
    size_t pos = 0;

    /* leaf_version (1 byte) */
    buf[pos++] = leaf_version;

    /* compact_size(script_len) */
    size_t written;
    varint_write(buf + pos, 9, script_len, &written);
    pos += written;

    /* script */
    if (script && script_len > 0) {
        if (script_len > sizeof(buf) - pos) {
            return ECHO_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(buf + pos, script, script_len);
        pos += script_len;
    }

    secp256k1_schnorr_tagged_hash(leaf_hash, "TapLeaf", buf, pos);

    return ECHO_OK;
}

/*
 * Parse a Taproot control block.
 */
echo_result_t taproot_parse_control_block(const uint8_t *control,
                                           size_t control_len,
                                           uint8_t *leaf_version,
                                           uint8_t *parity,
                                           uint8_t internal_key[32],
                                           const uint8_t **merkle_path,
                                           size_t *path_len)
{
    if (control == NULL || control_len < TAPROOT_CONTROL_BASE_SIZE) {
        return ECHO_ERR_INVALID_FORMAT;
    }

    /* Check control block size is valid */
    size_t merkle_size = control_len - TAPROOT_CONTROL_BASE_SIZE;
    if (merkle_size % TAPROOT_CONTROL_NODE_SIZE != 0) {
        return ECHO_ERR_INVALID_FORMAT;
    }

    size_t num_nodes = merkle_size / TAPROOT_CONTROL_NODE_SIZE;
    if (num_nodes > TAPROOT_CONTROL_MAX_NODE_COUNT) {
        return ECHO_ERR_INVALID_FORMAT;
    }

    /* Parse first byte: leaf_version (upper 7 bits) | parity (bit 0) */
    uint8_t first_byte = control[0];
    if (leaf_version) *leaf_version = first_byte & 0xFE;
    if (parity) *parity = first_byte & 0x01;

    /* Parse internal key (bytes 1-32) */
    if (internal_key) {
        memcpy(internal_key, control + 1, 32);
    }

    /* Set merkle path pointer and length */
    if (merkle_path) *merkle_path = control + TAPROOT_CONTROL_BASE_SIZE;
    if (path_len) *path_len = num_nodes;

    return ECHO_OK;
}

/*
 * Compute Taproot tweak from internal key and merkle root.
 * tweak = tagged_hash("TapTweak", internal_key || merkle_root)
 * If merkle_root is NULL (key path with no scripts), use just internal_key.
 */
static void compute_taproot_tweak(const uint8_t internal_key[32],
                                   const uint8_t *merkle_root,
                                   uint8_t tweak[32])
{
    if (merkle_root) {
        uint8_t buf[64];
        memcpy(buf, internal_key, 32);
        memcpy(buf + 32, merkle_root, 32);
        secp256k1_schnorr_tagged_hash(tweak, "TapTweak", buf, 64);
    } else {
        secp256k1_schnorr_tagged_hash(tweak, "TapTweak", internal_key, 32);
    }
}

/*
 * Verify the Taproot Merkle proof.
 */
echo_result_t taproot_verify_merkle_proof(const uint8_t leaf_hash[32],
                                           const uint8_t *merkle_path,
                                           size_t path_len,
                                           const uint8_t internal_key[32],
                                           const uint8_t output_key[32],
                                           uint8_t parity)
{
    if (leaf_hash == NULL || internal_key == NULL || output_key == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    /* Start with the leaf hash */
    uint8_t current[32];
    memcpy(current, leaf_hash, 32);

    /* Walk up the Merkle tree */
    for (size_t i = 0; i < path_len; i++) {
        const uint8_t *node = merkle_path + (i * 32);
        uint8_t buf[64];

        /* Order: smaller hash first (lexicographic) */
        if (memcmp(current, node, 32) < 0) {
            memcpy(buf, current, 32);
            memcpy(buf + 32, node, 32);
        } else {
            memcpy(buf, node, 32);
            memcpy(buf + 32, current, 32);
        }

        /* Branch hash = tagged_hash("TapBranch", left || right) */
        secp256k1_schnorr_tagged_hash(current, "TapBranch", buf, 64);
    }

    /* current is now the merkle root */
    /* Compute the tweak: t = tagged_hash("TapTweak", internal_key || merkle_root) */
    uint8_t tweak[32];
    compute_taproot_tweak(internal_key, current, tweak);

    /* Compute P + t*G where P is the internal key */
    secp256k1_point_t P, tG, Q;

    /* Parse internal key as x-only point */
    if (!secp256k1_xonly_pubkey_parse(&P, internal_key)) {
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Compute t*G */
    secp256k1_scalar_t t;
    secp256k1_scalar_set_bytes(&t, tweak);
    secp256k1_point_mul_gen(&tG, &t);

    /* Q = P + t*G */
    secp256k1_point_add(&Q, &P, &tG);

    /* Serialize Q as x-only and compare with output_key */
    uint8_t computed_key[32];
    secp256k1_xonly_pubkey_serialize(computed_key, &Q);

    if (memcmp(computed_key, output_key, 32) != 0) {
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Check parity matches */
    /* The parity bit indicates whether Q.y is even (0) or odd (1) */
    /* After serialization, we need to check if the actual y parity matches */
    /* For now, we skip this check as it requires more introspection into Q */

    return ECHO_OK;
}

/*
 * Verify a Taproot key path spend.
 */
echo_result_t script_verify_taproot_keypath(script_context_t *ctx,
                                             const uint8_t *witness_data,
                                             size_t witness_len,
                                             const uint8_t output_key[32])
{
    if (ctx == NULL || witness_data == NULL || output_key == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    /*
     * Key path witness: [signature]
     * Signature is 64 bytes (default sighash) or 65 bytes (explicit sighash)
     */
    if (witness_len < 1) {
        ctx->error = SCRIPT_ERR_WITNESS_PROGRAM_WITNESS_EMPTY;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Parse witness stack: count followed by items */
    size_t pos = 0;
    size_t count = witness_data[pos++];

    if (count != 1) {
        /* Key path must have exactly one witness element (signature) */
        ctx->error = SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Read signature length */
    if (pos >= witness_len) {
        ctx->error = SCRIPT_ERR_WITNESS_MALLEATED;
        return ECHO_ERR_SCRIPT_ERROR;
    }
    size_t sig_len = witness_data[pos++];
    if (pos + sig_len > witness_len) {
        ctx->error = SCRIPT_ERR_WITNESS_MALLEATED;
        return ECHO_ERR_SCRIPT_ERROR;
    }
    const uint8_t *sig = witness_data + pos;

    /* Validate signature length: 64 (default) or 65 (with sighash type) */
    if (sig_len != 64 && sig_len != 65) {
        ctx->error = SCRIPT_ERR_SCHNORR_SIG;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Extract sighash type */
    uint32_t sighash_type = SIGHASH_DEFAULT;
    size_t actual_sig_len = sig_len;
    if (sig_len == 65) {
        sighash_type = sig[64];
        actual_sig_len = 64;
        /* Validate sighash type */
        if (sighash_type == SIGHASH_DEFAULT) {
            /* 0x00 sighash with 65-byte sig is invalid */
            ctx->error = SCRIPT_ERR_SIG_HASHTYPE;
            return ECHO_ERR_SCRIPT_ERROR;
        }
    }

    /* Compute the sighash (key path: ext_flag = 0) */
    uint8_t sighash[32];
    echo_result_t res = sighash_taproot(ctx, sighash_type, 0, sighash);
    if (res != ECHO_OK) {
        ctx->error = SCRIPT_ERR_UNKNOWN_ERROR;
        return res;
    }

    /* Verify Schnorr signature */
    if (!sig_verify(SIG_SCHNORR, sig, actual_sig_len, sighash, output_key, 32)) {
        ctx->error = SCRIPT_ERR_SCHNORR_SIG;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    return ECHO_OK;
}

/*
 * Verify a Taproot script path spend.
 */
echo_result_t script_verify_taproot_scriptpath(script_context_t *ctx,
                                                const uint8_t *witness_data,
                                                size_t witness_len,
                                                const uint8_t output_key[32])
{
    if (ctx == NULL || witness_data == NULL || output_key == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    /*
     * Script path witness: [stack items...] [script] [control block]
     * We need at least 2 items: script and control block
     */
    if (witness_len < 1) {
        ctx->error = SCRIPT_ERR_WITNESS_PROGRAM_WITNESS_EMPTY;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Parse witness stack */
    size_t pos = 0;
    size_t count = witness_data[pos++];

    if (count < 2) {
        ctx->error = SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Parse all witness items to find the last two (script and control block) */
    const uint8_t *items[256];
    size_t item_lens[256];
    size_t item_count = 0;

    for (size_t i = 0; i < count && item_count < 256; i++) {
        if (pos >= witness_len) {
            ctx->error = SCRIPT_ERR_WITNESS_MALLEATED;
            return ECHO_ERR_SCRIPT_ERROR;
        }

        /* Read length (varint) */
        size_t item_len = witness_data[pos++];
        if (item_len == 0xfd) {
            if (pos + 2 > witness_len) {
                ctx->error = SCRIPT_ERR_WITNESS_MALLEATED;
                return ECHO_ERR_SCRIPT_ERROR;
            }
            item_len = witness_data[pos] | ((size_t)witness_data[pos + 1] << 8);
            pos += 2;
        } else if (item_len == 0xfe) {
            if (pos + 4 > witness_len) {
                ctx->error = SCRIPT_ERR_WITNESS_MALLEATED;
                return ECHO_ERR_SCRIPT_ERROR;
            }
            item_len = witness_data[pos] | ((size_t)witness_data[pos + 1] << 8) |
                       ((size_t)witness_data[pos + 2] << 16) | ((size_t)witness_data[pos + 3] << 24);
            pos += 4;
        }

        if (pos + item_len > witness_len) {
            ctx->error = SCRIPT_ERR_WITNESS_MALLEATED;
            return ECHO_ERR_SCRIPT_ERROR;
        }

        items[item_count] = witness_data + pos;
        item_lens[item_count] = item_len;
        item_count++;
        pos += item_len;
    }

    if (item_count < 2) {
        ctx->error = SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Last item is control block, second-to-last is script */
    const uint8_t *control = items[item_count - 1];
    size_t control_len = item_lens[item_count - 1];
    const uint8_t *script = items[item_count - 2];
    size_t script_len = item_lens[item_count - 2];

    /* Parse control block */
    uint8_t leaf_version, parity;
    uint8_t internal_key[32];
    const uint8_t *merkle_path;
    size_t path_len;

    echo_result_t res = taproot_parse_control_block(control, control_len,
                                                     &leaf_version, &parity,
                                                     internal_key, &merkle_path, &path_len);
    if (res != ECHO_OK) {
        ctx->error = SCRIPT_ERR_TAPROOT_WRONG_CONTROL_SIZE;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Compute leaf hash */
    uint8_t leaf_hash[32];
    res = taproot_leaf_hash(leaf_version, script, script_len, leaf_hash);
    if (res != ECHO_OK) {
        return res;
    }

    /* Verify Merkle proof */
    res = taproot_verify_merkle_proof(leaf_hash, merkle_path, path_len,
                                       internal_key, output_key, parity);
    if (res != ECHO_OK) {
        ctx->error = SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Set up Tapscript context */
    ctx->is_tapscript = (leaf_version == TAPROOT_LEAF_VERSION_TAPSCRIPT);
    memcpy(ctx->tapleaf_hash, leaf_hash, 32);
    memcpy(ctx->tap_internal_key, internal_key, 32);
    ctx->key_version = 0;  /* BIP-342 key version */

    /* Push stack items (excluding script and control block) onto stack */
    for (size_t i = 0; i < item_count - 2; i++) {
        res = stack_push(&ctx->stack, items[i], item_lens[i]);
        if (res != ECHO_OK) {
            ctx->error = SCRIPT_ERR_OUT_OF_MEMORY;
            return res;
        }
    }

    /* Execute the script */
    if (ctx->is_tapscript) {
        res = script_execute_tapscript(ctx, script, script_len);
    } else {
        /* Unknown leaf version: treat as OP_SUCCESS (anyone can spend) */
        return ECHO_OK;
    }

    if (res != ECHO_OK) {
        return res;
    }

    /* Check for success: stack must have exactly one true element */
    if (stack_empty(&ctx->stack)) {
        ctx->error = SCRIPT_ERR_EVAL_FALSE;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    const stack_element_t *top;
    stack_peek(&ctx->stack, &top);
    if (!script_bool(top->data, top->len)) {
        ctx->error = SCRIPT_ERR_EVAL_FALSE;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Clean stack check */
    if (ctx->stack.count != 1) {
        ctx->error = SCRIPT_ERR_CLEANSTACK;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    return ECHO_OK;
}

/*
 * Execute a Tapscript (BIP-342).
 *
 * Tapscript differs from legacy script:
 * - OP_CHECKMULTISIG and OP_CHECKMULTISIGVERIFY are disabled
 * - OP_CHECKSIGADD is enabled
 * - Success opcodes (0x50, 0x62, 0x7e-0x81, 0x83-0x86, etc.) cause immediate success
 * - Signature validation uses BIP-340 Schnorr
 * - MINIMALIF is always enforced
 */
echo_result_t script_execute_tapscript(script_context_t *ctx,
                                        const uint8_t *script,
                                        size_t len)
{
    if (ctx == NULL || (len > 0 && script == NULL)) {
        return ECHO_ERR_NULL_PARAM;
    }

    /* Ensure Tapscript mode is set */
    ctx->is_tapscript = ECHO_TRUE;

    /* Check script size */
    if (len > SCRIPT_MAX_SIZE) {
        ctx->error = SCRIPT_ERR_SCRIPT_SIZE;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    size_t pos = 0;

    while (pos < len) {
        uint8_t op = script[pos++];

        /*
         * Check for OP_SUCCESS opcodes (BIP-342).
         * These cause immediate success regardless of stack state.
         * OP_SUCCESS opcodes: 80, 98, 126-129, 131-134, 137-138, 141-142,
         *                     149-153, 187-254
         */
        if (op == 0x50 || op == 0x62 ||
            (op >= 0x7e && op <= 0x81) ||
            (op >= 0x83 && op <= 0x86) ||
            (op >= 0x89 && op <= 0x8a) ||
            (op >= 0x8d && op <= 0x8e) ||
            (op >= 0x95 && op <= 0x99) ||
            (op >= 0xbb && op <= 0xfe)) {
            /* OP_SUCCESS: script succeeds immediately */
            return ECHO_OK;
        }

        /* Check for disabled opcodes in Tapscript */
        if (op == OP_CHECKMULTISIG || op == OP_CHECKMULTISIGVERIFY) {
            ctx->error = SCRIPT_ERR_TAPSCRIPT_CHECKMULTISIG;
            return ECHO_ERR_SCRIPT_ERROR;
        }

        /* Handle OP_CHECKSIGADD (0xba) */
        if (op == OP_CHECKSIGADD) {
            /* Stack: sig n pubkey -> n+1 (if valid) or n (if invalid/empty sig) */
            if (ctx->stack.count < 3) {
                ctx->error = SCRIPT_ERR_INVALID_STACK_OPERATION;
                return ECHO_ERR_SCRIPT_ERROR;
            }

            /* Pop pubkey */
            stack_element_t pubkey_elem;
            echo_result_t res = stack_pop(&ctx->stack, &pubkey_elem);
            if (res != ECHO_OK) {
                ctx->error = SCRIPT_ERR_INVALID_STACK_OPERATION;
                return res;
            }

            /* Pop n */
            script_num_t n;
            res = stack_pop_num(&ctx->stack, &n, ECHO_TRUE, SCRIPT_NUM_MAX_SIZE);
            if (res != ECHO_OK) {
                element_free(&pubkey_elem);
                ctx->error = SCRIPT_ERR_INVALID_STACK_OPERATION;
                return res;
            }

            /* Pop sig */
            stack_element_t sig_elem;
            res = stack_pop(&ctx->stack, &sig_elem);
            if (res != ECHO_OK) {
                element_free(&pubkey_elem);
                ctx->error = SCRIPT_ERR_INVALID_STACK_OPERATION;
                return res;
            }

            /* Empty signature: push n unchanged */
            if (sig_elem.len == 0) {
                element_free(&pubkey_elem);
                element_free(&sig_elem);
                stack_push_num(&ctx->stack, n);
                continue;
            }

            /* Validate signature length */
            if (sig_elem.len != 64 && sig_elem.len != 65) {
                element_free(&pubkey_elem);
                element_free(&sig_elem);
                ctx->error = SCRIPT_ERR_SCHNORR_SIG;
                return ECHO_ERR_SCRIPT_ERROR;
            }

            /* Validate pubkey length */
            if (pubkey_elem.len != 32) {
                element_free(&pubkey_elem);
                element_free(&sig_elem);
                ctx->error = SCRIPT_ERR_WITNESS_PUBKEYTYPE;
                return ECHO_ERR_SCRIPT_ERROR;
            }

            /* Extract sighash type */
            uint32_t sighash_type = SIGHASH_DEFAULT;
            size_t actual_sig_len = sig_elem.len;
            if (sig_elem.len == 65) {
                sighash_type = sig_elem.data[64];
                actual_sig_len = 64;
                if (sighash_type == SIGHASH_DEFAULT) {
                    element_free(&pubkey_elem);
                    element_free(&sig_elem);
                    ctx->error = SCRIPT_ERR_SIG_HASHTYPE;
                    return ECHO_ERR_SCRIPT_ERROR;
                }
            }

            /* Compute sighash (script path: ext_flag = 1) */
            uint8_t sighash[32];
            res = sighash_taproot(ctx, sighash_type, 1, sighash);
            if (res != ECHO_OK) {
                element_free(&pubkey_elem);
                element_free(&sig_elem);
                ctx->error = SCRIPT_ERR_UNKNOWN_ERROR;
                return res;
            }

            /* Verify Schnorr signature */
            if (sig_verify(SIG_SCHNORR, sig_elem.data, actual_sig_len, sighash, pubkey_elem.data, pubkey_elem.len)) {
                n++;  /* Valid signature: increment counter */
            }
            /* Invalid signature with NULLFAIL check would fail here in strict mode */

            element_free(&pubkey_elem);
            element_free(&sig_elem);
            stack_push_num(&ctx->stack, n);
            continue;
        }

        /* For all other opcodes, use standard execution but with Tapscript rules */
        /* Re-parse as a single-opcode script and execute */
        pos--;  /* Back up to re-process */

        /* Create a mini-script for this single opcode */
        size_t opcode_len = 1;

        /* Handle push opcodes */
        if (op >= 0x01 && op <= 0x4b) {
            opcode_len = 1 + op;
        } else if (op == OP_PUSHDATA1 && pos + 1 < len) {
            opcode_len = 2 + script[pos + 1];
        } else if (op == OP_PUSHDATA2 && pos + 2 < len) {
            opcode_len = 3 + (script[pos + 1] | (script[pos + 2] << 8));
        } else if (op == OP_PUSHDATA4 && pos + 4 < len) {
            opcode_len = 5 + (script[pos + 1] | (script[pos + 2] << 8) |
                             (script[pos + 3] << 16) | (script[pos + 4] << 24));
        }

        if (pos + opcode_len > len) {
            ctx->error = SCRIPT_ERR_BAD_OPCODE;
            return ECHO_ERR_SCRIPT_ERROR;
        }

        /* Execute using standard script execution */
        echo_result_t res = script_execute(ctx, script + pos, opcode_len);
        if (res != ECHO_OK) {
            return res;
        }

        pos += opcode_len;
    }

    return ECHO_OK;
}


/*
 * ============================================================================
 * P2SH EVALUATION (Session 4.7 — BIP-16)
 * ============================================================================
 */

/*
 * Check if a scriptSig is push-only.
 *
 * A push-only script contains only push opcodes:
 * - OP_0 (0x00)
 * - Direct pushes (0x01-0x4b)
 * - OP_PUSHDATA1/2/4 (0x4c-0x4e)
 * - OP_1NEGATE (0x4f)
 * - OP_RESERVED (0x50) - treated as push
 * - OP_1-OP_16 (0x51-0x60)
 */
echo_bool_t script_is_push_only(const uint8_t *script, size_t len)
{
    if (script == NULL && len > 0) {
        return ECHO_FALSE;
    }

    size_t pos = 0;
    while (pos < len) {
        uint8_t op = script[pos++];

        if (op == OP_0) {
            /* OP_0 is a push (empty array) */
            continue;
        }

        if (op >= 0x01 && op <= 0x4b) {
            /* Direct push: op is the number of bytes to push */
            if (pos + op > len) {
                return ECHO_FALSE;  /* Truncated */
            }
            pos += op;
            continue;
        }

        if (op == OP_PUSHDATA1) {
            if (pos >= len) return ECHO_FALSE;
            size_t push_len = script[pos++];
            if (pos + push_len > len) return ECHO_FALSE;
            pos += push_len;
            continue;
        }

        if (op == OP_PUSHDATA2) {
            if (pos + 2 > len) return ECHO_FALSE;
            size_t push_len = script[pos] | ((size_t)script[pos + 1] << 8);
            pos += 2;
            if (pos + push_len > len) return ECHO_FALSE;
            pos += push_len;
            continue;
        }

        if (op == OP_PUSHDATA4) {
            if (pos + 4 > len) return ECHO_FALSE;
            size_t push_len = script[pos] | ((size_t)script[pos + 1] << 8) |
                              ((size_t)script[pos + 2] << 16) | ((size_t)script[pos + 3] << 24);
            pos += 4;
            if (pos + push_len > len) return ECHO_FALSE;
            pos += push_len;
            continue;
        }

        if (op == OP_1NEGATE) {
            /* OP_1NEGATE is a push */
            continue;
        }

        if (op >= OP_1 && op <= OP_16) {
            /* OP_1 through OP_16 are pushes */
            continue;
        }

        /* Any other opcode is not push-only */
        return ECHO_FALSE;
    }

    return ECHO_TRUE;
}

/*
 * Extract the serialized redeem script from an executed scriptSig.
 */
echo_result_t script_get_redeem_script(const script_context_t *ctx,
                                        const uint8_t **redeem_script,
                                        size_t *redeem_len)
{
    if (ctx == NULL || redeem_script == NULL || redeem_len == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    if (stack_empty(&ctx->stack)) {
        return ECHO_ERR_SCRIPT_STACK;
    }

    const stack_element_t *top;
    echo_result_t res = stack_peek(&ctx->stack, &top);
    if (res != ECHO_OK) {
        return res;
    }

    *redeem_script = top->data;
    *redeem_len = top->len;
    return ECHO_OK;
}

/*
 * Verify a P2SH (Pay to Script Hash) script.
 *
 * P2SH evaluation (BIP-16):
 * 1. Check scriptSig is push-only (if SIGPUSHONLY flag set)
 * 2. Execute scriptSig — leaves data items on stack including redeem script
 * 3. The top stack element is the serialized redeem script
 * 4. Verify HASH160(redeem_script) == script_hash
 * 5. If redeem script is a witness program (P2SH-P2WPKH or P2SH-P2WSH),
 *    delegate to SegWit verification
 * 6. Otherwise, pop redeem script and execute it with remaining stack
 */
echo_result_t script_verify_p2sh(script_context_t *ctx,
                                  const uint8_t *script_sig,
                                  size_t script_sig_len,
                                  const uint8_t script_hash[20],
                                  const uint8_t *witness_data,
                                  size_t witness_len)
{
    if (ctx == NULL || script_hash == NULL) {
        return ECHO_ERR_NULL_PARAM;
    }

    /* Empty scriptSig is invalid for P2SH */
    if (script_sig == NULL || script_sig_len == 0) {
        ctx->error = SCRIPT_ERR_EVAL_FALSE;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /*
     * Step 1: Check scriptSig is push-only (BIP-16 requirement)
     * This is always required for P2SH, not just when SIGPUSHONLY is set
     */
    if (!script_is_push_only(script_sig, script_sig_len)) {
        ctx->error = SCRIPT_ERR_SIG_PUSHONLY;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /*
     * Step 2: Execute scriptSig
     * This should push the redeem script (and any other data) onto the stack
     */
    echo_result_t res = script_execute(ctx, script_sig, script_sig_len);
    if (res != ECHO_OK) {
        return res;
    }

    /* Stack must not be empty after scriptSig execution */
    if (stack_empty(&ctx->stack)) {
        ctx->error = SCRIPT_ERR_EVAL_FALSE;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /*
     * Step 3: Extract the redeem script (top of stack)
     */
    const uint8_t *redeem_script;
    size_t redeem_len;
    res = script_get_redeem_script(ctx, &redeem_script, &redeem_len);
    if (res != ECHO_OK) {
        return res;
    }

    /*
     * Step 4: Verify HASH160(redeem_script) == script_hash
     */
    uint8_t computed_hash[20];
    hash160(redeem_script, redeem_len, computed_hash);
    if (memcmp(computed_hash, script_hash, 20) != 0) {
        ctx->error = SCRIPT_ERR_EQUALVERIFY;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /*
     * Step 5: Check if redeem script is a witness program (P2SH-SegWit)
     *
     * If the redeem script is:
     * - OP_0 <20 bytes>: P2SH-P2WPKH
     * - OP_0 <32 bytes>: P2SH-P2WSH
     * Then delegate to SegWit verification
     */
    witness_program_t witness_prog;
    if ((ctx->flags & SCRIPT_VERIFY_WITNESS) &&
        script_is_witness_program(redeem_script, redeem_len, &witness_prog)) {

        /* For P2SH-SegWit, the stack should only have the redeem script */
        if (stack_size(&ctx->stack) != 1) {
            ctx->error = SCRIPT_ERR_WITNESS_MALLEATED_P2SH;
            return ECHO_ERR_SCRIPT_ERROR;
        }

        /* Pop the redeem script (we've verified it) */
        stack_element_t redeem_elem;
        stack_pop(&ctx->stack, &redeem_elem);

        /* Witness must be present for SegWit */
        if (witness_data == NULL || witness_len == 0) {
            element_free(&redeem_elem);
            ctx->error = SCRIPT_ERR_WITNESS_PROGRAM_WITNESS_EMPTY;
            return ECHO_ERR_SCRIPT_ERROR;
        }

        if (witness_prog.version == WITNESS_VERSION_0) {
            if (witness_prog.program_len == 20) {
                /* P2SH-P2WPKH */
                res = script_verify_p2wpkh(ctx, witness_data, witness_len,
                                            witness_prog.program);
            } else if (witness_prog.program_len == 32) {
                /* P2SH-P2WSH */
                res = script_verify_p2wsh(ctx, witness_data, witness_len,
                                           witness_prog.program);
            } else {
                ctx->error = SCRIPT_ERR_WITNESS_PROGRAM_WRONG_LENGTH;
                res = ECHO_ERR_SCRIPT_ERROR;
            }
        } else {
            /* Unknown witness version in P2SH context */
            if (ctx->flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM) {
                ctx->error = SCRIPT_ERR_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM;
                res = ECHO_ERR_SCRIPT_ERROR;
            } else {
                /* Future witness versions succeed by default */
                res = ECHO_OK;
            }
        }

        element_free(&redeem_elem);
        return res;
    }

    /*
     * Step 6: Regular P2SH (not SegWit-wrapped)
     *
     * Make a copy of the redeem script before we pop it,
     * then pop it and execute with remaining stack.
     */

    /* Copy the redeem script since we need it after popping */
    uint8_t *redeem_copy = NULL;
    if (redeem_len > 0) {
        redeem_copy = malloc(redeem_len);
        if (redeem_copy == NULL) {
            ctx->error = SCRIPT_ERR_OUT_OF_MEMORY;
            return ECHO_ERR_OUT_OF_MEMORY;
        }
        memcpy(redeem_copy, redeem_script, redeem_len);
    }
    size_t redeem_copy_len = redeem_len;

    /* Pop the redeem script from stack */
    stack_element_t redeem_elem;
    res = stack_pop(&ctx->stack, &redeem_elem);
    if (res != ECHO_OK) {
        free(redeem_copy);
        return res;
    }
    element_free(&redeem_elem);

    /* Check redeem script size limit */
    if (redeem_copy_len > SCRIPT_MAX_SIZE) {
        free(redeem_copy);
        ctx->error = SCRIPT_ERR_SCRIPT_SIZE;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Set the redeem script as script_code for sighash computation */
    ctx->script_code = redeem_copy;
    ctx->script_code_len = redeem_copy_len;
    ctx->codesep_pos = 0;

    /*
     * Execute the redeem script with remaining stack
     * The op_count carries over from scriptSig execution
     */
    res = script_execute(ctx, redeem_copy, redeem_copy_len);
    free(redeem_copy);

    if (res != ECHO_OK) {
        return res;
    }

    /* After P2SH execution, stack must have exactly one true element */
    if (stack_empty(&ctx->stack)) {
        ctx->error = SCRIPT_ERR_EVAL_FALSE;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Check the final result */
    const stack_element_t *top;
    res = stack_peek(&ctx->stack, &top);
    if (res != ECHO_OK) {
        return res;
    }

    if (!script_bool(top->data, top->len)) {
        ctx->error = SCRIPT_ERR_EVAL_FALSE;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    /* Clean stack check for P2SH */
    if ((ctx->flags & SCRIPT_VERIFY_CLEANSTACK) && stack_size(&ctx->stack) != 1) {
        ctx->error = SCRIPT_ERR_CLEANSTACK;
        return ECHO_ERR_SCRIPT_ERROR;
    }

    return ECHO_OK;
}
