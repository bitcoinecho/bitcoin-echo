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
