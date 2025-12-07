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
        case SCRIPT_ERR_WITNESS_PROGRAM_WRONG_LENGTH: return "Wrong witness program length";
        case SCRIPT_ERR_WITNESS_PROGRAM_WITNESS_EMPTY: return "Witness program requires witness";
        case SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH: return "Witness program mismatch";
        case SCRIPT_ERR_WITNESS_MALLEATED: return "Witness malleated";
        case SCRIPT_ERR_WITNESS_MALLEATED_P2SH: return "Witness malleated (P2SH)";
        case SCRIPT_ERR_WITNESS_UNEXPECTED: return "Unexpected witness";
        case SCRIPT_ERR_WITNESS_PUBKEYTYPE: return "Invalid witness public key type";
        case SCRIPT_ERR_TAPROOT_WRONG_CONTROL_SIZE: return "Wrong taproot control size";
        case SCRIPT_ERR_TAPSCRIPT_VALIDATION_WEIGHT: return "Tapscript validation weight exceeded";
        case SCRIPT_ERR_TAPSCRIPT_CHECKMULTISIG: return "CHECKMULTISIG not in tapscript";
        case SCRIPT_ERR_TAPSCRIPT_MINIMALIF: return "Tapscript requires minimal IF";
        case SCRIPT_ERR_OUT_OF_MEMORY: return "Out of memory";
        default: return "Unknown error";
    }
}
