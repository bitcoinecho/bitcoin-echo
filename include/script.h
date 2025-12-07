/*
 * Bitcoin Echo — Script Data Structures
 *
 * This header defines Bitcoin Script opcodes, structures, and types.
 * Bitcoin Script is a stack-based language with no loops, used to
 * define conditions under which outputs can be spent.
 *
 * Script types supported:
 *   - P2PK   (Pay to Public Key) — legacy, rarely used
 *   - P2PKH  (Pay to Public Key Hash) — legacy addresses (1...)
 *   - P2SH   (Pay to Script Hash) — BIP-16 (3...)
 *   - P2WPKH (Pay to Witness Public Key Hash) — SegWit v0 (bc1q...)
 *   - P2WSH  (Pay to Witness Script Hash) — SegWit v0 (bc1q...)
 *   - P2TR   (Pay to Taproot) — SegWit v1 (bc1p...)
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_SCRIPT_H
#define ECHO_SCRIPT_H

#include "echo_types.h"

/*
 * Script size limits (consensus).
 */
#define SCRIPT_MAX_SIZE           10000   /* Max script size in bytes */
#define SCRIPT_MAX_OPS            201     /* Max non-push operations */
#define SCRIPT_MAX_STACK_SIZE     1000    /* Max stack elements */
#define SCRIPT_MAX_ELEMENT_SIZE   520     /* Max size of stack element */
#define SCRIPT_MAX_PUBKEYS_PER_MULTISIG 20 /* Max keys in CHECKMULTISIG */
#define SCRIPT_MAX_WITNESS_SIZE   4000000 /* Max witness size */

/*
 * Standard script sizes (for type detection).
 */
#define SCRIPT_P2PKH_SIZE    25   /* OP_DUP OP_HASH160 <20> OP_EQUALVERIFY OP_CHECKSIG */
#define SCRIPT_P2SH_SIZE     23   /* OP_HASH160 <20> OP_EQUAL */
#define SCRIPT_P2WPKH_SIZE   22   /* OP_0 <20> */
#define SCRIPT_P2WSH_SIZE    34   /* OP_0 <32> */
#define SCRIPT_P2TR_SIZE     34   /* OP_1 <32> */

/*
 * Bitcoin Script opcodes.
 *
 * This enumeration includes all opcodes defined in Bitcoin's history:
 *   - Original opcodes from Bitcoin 0.1
 *   - Disabled opcodes (execution fails if encountered)
 *   - BIP-65 OP_CHECKLOCKTIMEVERIFY
 *   - BIP-112 OP_CHECKSEQUENCEVERIFY
 *   - BIP-342 OP_CHECKSIGADD (Tapscript)
 *
 * Opcodes are grouped by function for clarity.
 */
typedef enum {
    /*
     * Push value opcodes (0x00 - 0x60)
     */

    /* OP_0 pushes an empty byte array (which evaluates as false) */
    OP_0                    = 0x00,
    OP_FALSE                = 0x00,  /* Alias for OP_0 */

    /* OP_PUSHDATA: next N bytes specify the number of bytes to push */
    /* 0x01-0x4B: Push next 1-75 bytes directly */
    /* (These are not named opcodes; the value itself is the push size) */
    OP_PUSHDATA1            = 0x4c,  /* Next 1 byte is length, then data */
    OP_PUSHDATA2            = 0x4d,  /* Next 2 bytes (LE) is length, then data */
    OP_PUSHDATA4            = 0x4e,  /* Next 4 bytes (LE) is length, then data */

    OP_1NEGATE              = 0x4f,  /* Push -1 onto stack */

    OP_RESERVED             = 0x50,  /* Reserved (tx invalid if executed) */

    /* OP_1 through OP_16 push the values 1-16 */
    OP_1                    = 0x51,
    OP_TRUE                 = 0x51,  /* Alias for OP_1 */
    OP_2                    = 0x52,
    OP_3                    = 0x53,
    OP_4                    = 0x54,
    OP_5                    = 0x55,
    OP_6                    = 0x56,
    OP_7                    = 0x57,
    OP_8                    = 0x58,
    OP_9                    = 0x59,
    OP_10                   = 0x5a,
    OP_11                   = 0x5b,
    OP_12                   = 0x5c,
    OP_13                   = 0x5d,
    OP_14                   = 0x5e,
    OP_15                   = 0x5f,
    OP_16                   = 0x60,

    /*
     * Flow control opcodes (0x61 - 0x6A)
     */
    OP_NOP                  = 0x61,
    OP_VER                  = 0x62,  /* Reserved (tx invalid if executed) */
    OP_IF                   = 0x63,
    OP_NOTIF                = 0x64,
    OP_VERIF                = 0x65,  /* Reserved (tx ALWAYS invalid) */
    OP_VERNOTIF             = 0x66,  /* Reserved (tx ALWAYS invalid) */
    OP_ELSE                 = 0x67,
    OP_ENDIF                = 0x68,
    OP_VERIFY               = 0x69,
    OP_RETURN               = 0x6a,  /* Marks tx as provably unspendable */

    /*
     * Stack opcodes (0x6B - 0x7E)
     */
    OP_TOALTSTACK           = 0x6b,
    OP_FROMALTSTACK         = 0x6c,
    OP_2DROP                = 0x6d,
    OP_2DUP                 = 0x6e,
    OP_3DUP                 = 0x6f,
    OP_2OVER                = 0x70,
    OP_2ROT                 = 0x71,
    OP_2SWAP                = 0x72,
    OP_IFDUP                = 0x73,
    OP_DEPTH                = 0x74,
    OP_DROP                 = 0x75,
    OP_DUP                  = 0x76,
    OP_NIP                  = 0x77,
    OP_OVER                 = 0x78,
    OP_PICK                 = 0x79,
    OP_ROLL                 = 0x7a,
    OP_ROT                  = 0x7b,
    OP_SWAP                 = 0x7c,
    OP_TUCK                 = 0x7d,

    /*
     * Splice opcodes (0x7E - 0x82) — ALL DISABLED
     * Executing any of these makes the transaction invalid.
     */
    OP_CAT                  = 0x7e,  /* DISABLED */
    OP_SUBSTR               = 0x7f,  /* DISABLED */
    OP_LEFT                 = 0x80,  /* DISABLED */
    OP_RIGHT                = 0x81,  /* DISABLED */
    OP_SIZE                 = 0x82,  /* Returns length of top stack item */

    /*
     * Bitwise logic opcodes (0x83 - 0x88)
     */
    OP_INVERT               = 0x83,  /* DISABLED */
    OP_AND                  = 0x84,  /* DISABLED */
    OP_OR                   = 0x85,  /* DISABLED */
    OP_XOR                  = 0x86,  /* DISABLED */
    OP_EQUAL                = 0x87,
    OP_EQUALVERIFY          = 0x88,

    OP_RESERVED1            = 0x89,  /* Reserved (tx invalid if executed) */
    OP_RESERVED2            = 0x8a,  /* Reserved (tx invalid if executed) */

    /*
     * Arithmetic opcodes (0x8B - 0xA5)
     * All arithmetic is done on 4-byte signed integers (little-endian).
     * Overflow results in script failure.
     */
    OP_1ADD                 = 0x8b,
    OP_1SUB                 = 0x8c,
    OP_2MUL                 = 0x8d,  /* DISABLED */
    OP_2DIV                 = 0x8e,  /* DISABLED */
    OP_NEGATE               = 0x8f,
    OP_ABS                  = 0x90,
    OP_NOT                  = 0x91,
    OP_0NOTEQUAL            = 0x92,
    OP_ADD                  = 0x93,
    OP_SUB                  = 0x94,
    OP_MUL                  = 0x95,  /* DISABLED */
    OP_DIV                  = 0x96,  /* DISABLED */
    OP_MOD                  = 0x97,  /* DISABLED */
    OP_LSHIFT               = 0x98,  /* DISABLED */
    OP_RSHIFT               = 0x99,  /* DISABLED */
    OP_BOOLAND              = 0x9a,
    OP_BOOLOR               = 0x9b,
    OP_NUMEQUAL             = 0x9c,
    OP_NUMEQUALVERIFY       = 0x9d,
    OP_NUMNOTEQUAL          = 0x9e,
    OP_LESSTHAN             = 0x9f,
    OP_GREATERTHAN          = 0xa0,
    OP_LESSTHANOREQUAL      = 0xa1,
    OP_GREATERTHANOREQUAL   = 0xa2,
    OP_MIN                  = 0xa3,
    OP_MAX                  = 0xa4,
    OP_WITHIN               = 0xa5,

    /*
     * Cryptographic opcodes (0xA6 - 0xAF)
     */
    OP_RIPEMD160            = 0xa6,
    OP_SHA1                 = 0xa7,
    OP_SHA256               = 0xa8,
    OP_HASH160              = 0xa9,  /* RIPEMD160(SHA256(x)) */
    OP_HASH256              = 0xaa,  /* SHA256(SHA256(x)) */
    OP_CODESEPARATOR        = 0xab,
    OP_CHECKSIG             = 0xac,
    OP_CHECKSIGVERIFY       = 0xad,
    OP_CHECKMULTISIG        = 0xae,
    OP_CHECKMULTISIGVERIFY  = 0xaf,

    /*
     * Reserved/expansion opcodes (0xB0 - 0xB9)
     * NOP1-10 reserved for future soft-fork upgrades.
     * Some have been repurposed by BIPs.
     */
    OP_NOP1                 = 0xb0,
    OP_CHECKLOCKTIMEVERIFY  = 0xb1,  /* BIP-65: OP_NOP2 repurposed */
    OP_NOP2                 = 0xb1,  /* Alias (pre-BIP-65) */
    OP_CHECKSEQUENCEVERIFY  = 0xb2,  /* BIP-112: OP_NOP3 repurposed */
    OP_NOP3                 = 0xb2,  /* Alias (pre-BIP-112) */
    OP_NOP4                 = 0xb3,
    OP_NOP5                 = 0xb4,
    OP_NOP6                 = 0xb5,
    OP_NOP7                 = 0xb6,
    OP_NOP8                 = 0xb7,
    OP_NOP9                 = 0xb8,
    OP_NOP10                = 0xb9,

    /*
     * Tapscript opcodes (BIP-342)
     */
    OP_CHECKSIGADD          = 0xba,  /* BIP-342: For batch signature verification */

    /*
     * Reserved for internal use / invalid opcodes (0xBB - 0xFF)
     * OP_INVALIDOPCODE is used as a sentinel value.
     */
    OP_INVALIDOPCODE        = 0xff

} script_opcode_t;

/*
 * Script type enumeration.
 * Identifies the pattern of a scriptPubKey for special handling.
 */
typedef enum {
    SCRIPT_TYPE_UNKNOWN      = 0,  /* Non-standard or unrecognized */
    SCRIPT_TYPE_P2PK         = 1,  /* Pay to Public Key */
    SCRIPT_TYPE_P2PKH        = 2,  /* Pay to Public Key Hash */
    SCRIPT_TYPE_P2SH         = 3,  /* Pay to Script Hash (BIP-16) */
    SCRIPT_TYPE_P2WPKH       = 4,  /* Pay to Witness Public Key Hash (SegWit v0) */
    SCRIPT_TYPE_P2WSH        = 5,  /* Pay to Witness Script Hash (SegWit v0) */
    SCRIPT_TYPE_P2TR         = 6,  /* Pay to Taproot (SegWit v1, BIP-341) */
    SCRIPT_TYPE_WITNESS_UNKNOWN = 7, /* Unknown witness version (future) */
    SCRIPT_TYPE_MULTISIG     = 8,  /* Bare multisig */
    SCRIPT_TYPE_NULL_DATA    = 9,  /* OP_RETURN data carrier */
} script_type_t;

/*
 * Witness version for SegWit outputs.
 */
#define WITNESS_VERSION_0    0   /* BIP-141: P2WPKH, P2WSH */
#define WITNESS_VERSION_1    1   /* BIP-341: P2TR (Taproot) */
#define WITNESS_VERSION_MAX  16  /* Maximum witness version */

/*
 * Script structure.
 * Represents a Bitcoin script as raw bytes.
 * The script does not own its data — caller must manage memory.
 */
typedef struct {
    const uint8_t *data;   /* Script bytes */
    size_t         len;    /* Script length */
} script_t;

/*
 * Mutable script structure.
 * Owns its data, must be freed with script_free().
 */
typedef struct {
    uint8_t *data;         /* Script bytes (owned) */
    size_t   len;          /* Script length */
    size_t   capacity;     /* Allocated capacity */
} script_mut_t;

/*
 * Parsed opcode with its data.
 * Used when iterating through a script.
 */
typedef struct {
    script_opcode_t op;    /* The opcode */
    const uint8_t  *data;  /* Push data (NULL if not a push op) */
    size_t          len;   /* Length of push data */
} script_op_t;

/*
 * Script iterator for parsing opcodes.
 */
typedef struct {
    const uint8_t *script;  /* Script being iterated */
    size_t         script_len;
    size_t         pos;     /* Current position */
    echo_bool_t    error;   /* Set if parse error occurred */
} script_iter_t;

/*
 * Witness program structure.
 * A witness program is a scriptPubKey of the form: <version> <program>
 */
typedef struct {
    int     version;       /* Witness version (0-16) */
    uint8_t program[40];   /* Witness program (20 or 32 bytes typically) */
    size_t  program_len;   /* Length of program */
} witness_program_t;


/*
 * Initialize a mutable script to empty state.
 *
 * Parameters:
 *   script - Script to initialize
 */
void script_mut_init(script_mut_t *script);

/*
 * Free memory owned by a mutable script.
 *
 * Parameters:
 *   script - Script to free (structure itself not freed)
 */
void script_mut_free(script_mut_t *script);

/*
 * Determine the type of a script.
 *
 * This function examines a scriptPubKey and identifies its type
 * based on known patterns (P2PKH, P2SH, P2WPKH, P2WSH, P2TR, etc.)
 *
 * Parameters:
 *   data - Script bytes
 *   len  - Script length
 *
 * Returns:
 *   The script type, or SCRIPT_TYPE_UNKNOWN if not recognized
 */
script_type_t script_classify(const uint8_t *data, size_t len);

/*
 * Check if a script is a witness program.
 *
 * A witness program has the form: <version> <program>
 * where version is OP_0 to OP_16 and program is 2-40 bytes.
 *
 * Parameters:
 *   data    - Script bytes
 *   len     - Script length
 *   witness - Output: parsed witness program (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if script is a witness program
 */
echo_bool_t script_is_witness_program(const uint8_t *data, size_t len,
                                       witness_program_t *witness);

/*
 * Check if a script is P2PKH (Pay to Public Key Hash).
 * Pattern: OP_DUP OP_HASH160 <20 bytes> OP_EQUALVERIFY OP_CHECKSIG
 *
 * Parameters:
 *   data - Script bytes
 *   len  - Script length
 *   hash - Output: the 20-byte pubkey hash (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if P2PKH, ECHO_FALSE otherwise
 */
echo_bool_t script_is_p2pkh(const uint8_t *data, size_t len, hash160_t *hash);

/*
 * Check if a script is P2SH (Pay to Script Hash).
 * Pattern: OP_HASH160 <20 bytes> OP_EQUAL
 *
 * Parameters:
 *   data - Script bytes
 *   len  - Script length
 *   hash - Output: the 20-byte script hash (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if P2SH, ECHO_FALSE otherwise
 */
echo_bool_t script_is_p2sh(const uint8_t *data, size_t len, hash160_t *hash);

/*
 * Check if a script is P2WPKH (Pay to Witness Public Key Hash).
 * Pattern: OP_0 <20 bytes>
 *
 * Parameters:
 *   data - Script bytes
 *   len  - Script length
 *   hash - Output: the 20-byte witness pubkey hash (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if P2WPKH, ECHO_FALSE otherwise
 */
echo_bool_t script_is_p2wpkh(const uint8_t *data, size_t len, hash160_t *hash);

/*
 * Check if a script is P2WSH (Pay to Witness Script Hash).
 * Pattern: OP_0 <32 bytes>
 *
 * Parameters:
 *   data - Script bytes
 *   len  - Script length
 *   hash - Output: the 32-byte witness script hash (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if P2WSH, ECHO_FALSE otherwise
 */
echo_bool_t script_is_p2wsh(const uint8_t *data, size_t len, hash256_t *hash);

/*
 * Check if a script is P2TR (Pay to Taproot).
 * Pattern: OP_1 <32 bytes>
 *
 * Parameters:
 *   data - Script bytes
 *   len  - Script length
 *   key  - Output: the 32-byte x-only public key (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if P2TR, ECHO_FALSE otherwise
 */
echo_bool_t script_is_p2tr(const uint8_t *data, size_t len, hash256_t *key);

/*
 * Check if a script is P2PK (Pay to Public Key).
 * Pattern: <33 or 65 bytes pubkey> OP_CHECKSIG
 *
 * Parameters:
 *   data       - Script bytes
 *   len        - Script length
 *   pubkey     - Output: pointer to public key in script (may be NULL)
 *   pubkey_len - Output: length of public key (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if P2PK, ECHO_FALSE otherwise
 */
echo_bool_t script_is_p2pk(const uint8_t *data, size_t len,
                           const uint8_t **pubkey, size_t *pubkey_len);

/*
 * Check if a script is OP_RETURN (null data / unspendable).
 * Pattern: OP_RETURN [data...]
 *
 * Parameters:
 *   data - Script bytes
 *   len  - Script length
 *
 * Returns:
 *   ECHO_TRUE if OP_RETURN script, ECHO_FALSE otherwise
 */
echo_bool_t script_is_op_return(const uint8_t *data, size_t len);

/*
 * Initialize a script iterator.
 *
 * Parameters:
 *   iter   - Iterator to initialize
 *   script - Script bytes to iterate
 *   len    - Script length
 */
void script_iter_init(script_iter_t *iter, const uint8_t *script, size_t len);

/*
 * Get the next opcode from a script.
 *
 * Parameters:
 *   iter - Script iterator
 *   op   - Output: parsed opcode and data
 *
 * Returns:
 *   ECHO_TRUE if an opcode was read, ECHO_FALSE if end or error
 */
echo_bool_t script_iter_next(script_iter_t *iter, script_op_t *op);

/*
 * Check if iterator encountered an error.
 *
 * Parameters:
 *   iter - Script iterator
 *
 * Returns:
 *   ECHO_TRUE if error occurred, ECHO_FALSE otherwise
 */
echo_bool_t script_iter_error(const script_iter_t *iter);

/*
 * Check if an opcode is disabled (makes transaction invalid).
 *
 * Disabled opcodes include: OP_CAT, OP_SUBSTR, OP_LEFT, OP_RIGHT,
 * OP_INVERT, OP_AND, OP_OR, OP_XOR, OP_2MUL, OP_2DIV, OP_MUL,
 * OP_DIV, OP_MOD, OP_LSHIFT, OP_RSHIFT.
 *
 * Parameters:
 *   op - Opcode to check
 *
 * Returns:
 *   ECHO_TRUE if disabled, ECHO_FALSE otherwise
 */
echo_bool_t script_opcode_disabled(script_opcode_t op);

/*
 * Check if a byte value is a push opcode.
 *
 * Push opcodes are: 0x00-0x4e (OP_0, direct push 1-75, PUSHDATA1/2/4)
 *
 * Parameters:
 *   op - Opcode to check
 *
 * Returns:
 *   ECHO_TRUE if push opcode, ECHO_FALSE otherwise
 */
echo_bool_t script_opcode_is_push(uint8_t op);

/*
 * Get the name of an opcode as a string.
 *
 * Parameters:
 *   op - Opcode
 *
 * Returns:
 *   Static string with opcode name, or "OP_UNKNOWN" for undefined opcodes
 */
const char *script_opcode_name(script_opcode_t op);

/*
 * Count the number of signature operations in a script.
 *
 * This counts OP_CHECKSIG, OP_CHECKSIGVERIFY, OP_CHECKMULTISIG,
 * OP_CHECKMULTISIGVERIFY (with accurate counting based on keys).
 *
 * Parameters:
 *   data      - Script bytes
 *   len       - Script length
 *   accurate  - If true, count actual keys in multisig; otherwise use max (20)
 *
 * Returns:
 *   Number of signature operations
 */
size_t script_sigops_count(const uint8_t *data, size_t len, echo_bool_t accurate);

/*
 * Compute the minimum push size for a given data length.
 *
 * Returns the number of bytes needed to push data of the given length.
 *
 * Parameters:
 *   data_len - Length of data to push
 *
 * Returns:
 *   Total bytes needed (opcode + length prefix + data)
 */
size_t script_push_size(size_t data_len);

#endif /* ECHO_SCRIPT_H */
