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
#include <stdint.h>

/* Forward declaration for transaction type */
struct tx_s;
typedef struct tx_s tx_t;

/*
 * ============================================================================
 * SIGHASH TYPES (BIP-143)
 * ============================================================================
 *
 * The sighash type determines which parts of the transaction are signed.
 * This allows for various spending conditions and transaction construction.
 */

/*
 * Base sighash types (lower 5 bits).
 */
#define SIGHASH_ALL 0x01    /* Sign all inputs and outputs */
#define SIGHASH_NONE 0x02   /* Sign all inputs, no outputs */
#define SIGHASH_SINGLE 0x03 /* Sign all inputs, only matching output */

/*
 * Sighash modifier (bit 7).
 */
#define SIGHASH_ANYONECANPAY 0x80 /* Only sign own input */

/*
 * Combined sighash types.
 */
#define SIGHASH_ALL_ANYONECANPAY (SIGHASH_ALL | SIGHASH_ANYONECANPAY)
#define SIGHASH_NONE_ANYONECANPAY (SIGHASH_NONE | SIGHASH_ANYONECANPAY)
#define SIGHASH_SINGLE_ANYONECANPAY (SIGHASH_SINGLE | SIGHASH_ANYONECANPAY)

/*
 * Taproot sighash type (BIP-341).
 */
#define SIGHASH_DEFAULT 0x00 /* Taproot default = SIGHASH_ALL semantics */

/*
 * ============================================================================
 * TAPROOT CONSTANTS (BIP-341/342)
 * ============================================================================
 */

/*
 * Taproot leaf versions.
 * The leaf version is encoded in the control block with the parity bit.
 */
#define TAPROOT_LEAF_VERSION_TAPSCRIPT 0xC0 /* BIP-342 Tapscript (v0) */

/*
 * Taproot control block limits.
 */
#define TAPROOT_CONTROL_BASE_SIZE 33 /* leaf_version (1) + internal_key (32)   \
                                      */
#define TAPROOT_CONTROL_NODE_SIZE 32 /* Each Merkle node is 32 bytes */
#define TAPROOT_CONTROL_MAX_NODE_COUNT 128 /* Max Merkle tree depth */
#define TAPROOT_CONTROL_MAX_SIZE                                               \
  (TAPROOT_CONTROL_BASE_SIZE +                                                 \
   TAPROOT_CONTROL_NODE_SIZE * TAPROOT_CONTROL_MAX_NODE_COUNT)

/*
 * Tapscript limits (more restrictive than legacy).
 */
#define TAPSCRIPT_MAX_STACK_ELEMENT_SIZE 520 /* Same as legacy */
#define TAPSCRIPT_VALIDATION_WEIGHT 50       /* Weight units per sigop */

/*
 * Script size limits (consensus).
 */
#define SCRIPT_MAX_SIZE 10000              /* Max script size in bytes */
#define SCRIPT_MAX_OPS 201                 /* Max non-push operations */
#define SCRIPT_MAX_STACK_SIZE 1000         /* Max stack elements */
#define SCRIPT_MAX_ELEMENT_SIZE 520        /* Max size of stack element */
#define SCRIPT_MAX_PUBKEYS_PER_MULTISIG 20 /* Max keys in CHECKMULTISIG */
#define SCRIPT_MAX_WITNESS_SIZE 4000000    /* Max witness size */

/*
 * Standard script sizes (for type detection).
 */
#define SCRIPT_P2PKH_SIZE                                                      \
  25 /* OP_DUP OP_HASH160 <20> OP_EQUALVERIFY OP_CHECKSIG */
#define SCRIPT_P2SH_SIZE 23   /* OP_HASH160 <20> OP_EQUAL */
#define SCRIPT_P2WPKH_SIZE 22 /* OP_0 <20> */
#define SCRIPT_P2WSH_SIZE 34  /* OP_0 <32> */
#define SCRIPT_P2TR_SIZE 34   /* OP_1 <32> */

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
  OP_0 = 0x00,
  OP_FALSE = 0x00, /* Alias for OP_0 */

  /* OP_PUSHDATA: next N bytes specify the number of bytes to push */
  /* 0x01-0x4B: Push next 1-75 bytes directly */
  /* (These are not named opcodes; the value itself is the push size) */
  OP_PUSHDATA1 = 0x4c, /* Next 1 byte is length, then data */
  OP_PUSHDATA2 = 0x4d, /* Next 2 bytes (LE) is length, then data */
  OP_PUSHDATA4 = 0x4e, /* Next 4 bytes (LE) is length, then data */

  OP_1NEGATE = 0x4f, /* Push -1 onto stack */

  OP_RESERVED = 0x50, /* Reserved (tx invalid if executed) */

  /* OP_1 through OP_16 push the values 1-16 */
  OP_1 = 0x51,
  OP_TRUE = 0x51, /* Alias for OP_1 */
  OP_2 = 0x52,
  OP_3 = 0x53,
  OP_4 = 0x54,
  OP_5 = 0x55,
  OP_6 = 0x56,
  OP_7 = 0x57,
  OP_8 = 0x58,
  OP_9 = 0x59,
  OP_10 = 0x5a,
  OP_11 = 0x5b,
  OP_12 = 0x5c,
  OP_13 = 0x5d,
  OP_14 = 0x5e,
  OP_15 = 0x5f,
  OP_16 = 0x60,

  /*
   * Flow control opcodes (0x61 - 0x6A)
   */
  OP_NOP = 0x61,
  OP_VER = 0x62, /* Reserved (tx invalid if executed) */
  OP_IF = 0x63,
  OP_NOTIF = 0x64,
  OP_VERIF = 0x65,    /* Reserved (tx ALWAYS invalid) */
  OP_VERNOTIF = 0x66, /* Reserved (tx ALWAYS invalid) */
  OP_ELSE = 0x67,
  OP_ENDIF = 0x68,
  OP_VERIFY = 0x69,
  OP_RETURN = 0x6a, /* Marks tx as provably unspendable */

  /*
   * Stack opcodes (0x6B - 0x7E)
   */
  OP_TOALTSTACK = 0x6b,
  OP_FROMALTSTACK = 0x6c,
  OP_2DROP = 0x6d,
  OP_2DUP = 0x6e,
  OP_3DUP = 0x6f,
  OP_2OVER = 0x70,
  OP_2ROT = 0x71,
  OP_2SWAP = 0x72,
  OP_IFDUP = 0x73,
  OP_DEPTH = 0x74,
  OP_DROP = 0x75,
  OP_DUP = 0x76,
  OP_NIP = 0x77,
  OP_OVER = 0x78,
  OP_PICK = 0x79,
  OP_ROLL = 0x7a,
  OP_ROT = 0x7b,
  OP_SWAP = 0x7c,
  OP_TUCK = 0x7d,

  /*
   * Splice opcodes (0x7E - 0x82) — ALL DISABLED
   * Executing any of these makes the transaction invalid.
   */
  OP_CAT = 0x7e,    /* DISABLED */
  OP_SUBSTR = 0x7f, /* DISABLED */
  OP_LEFT = 0x80,   /* DISABLED */
  OP_RIGHT = 0x81,  /* DISABLED */
  OP_SIZE = 0x82,   /* Returns length of top stack item */

  /*
   * Bitwise logic opcodes (0x83 - 0x88)
   */
  OP_INVERT = 0x83, /* DISABLED */
  OP_AND = 0x84,    /* DISABLED */
  OP_OR = 0x85,     /* DISABLED */
  OP_XOR = 0x86,    /* DISABLED */
  OP_EQUAL = 0x87,
  OP_EQUALVERIFY = 0x88,

  OP_RESERVED1 = 0x89, /* Reserved (tx invalid if executed) */
  OP_RESERVED2 = 0x8a, /* Reserved (tx invalid if executed) */

  /*
   * Arithmetic opcodes (0x8B - 0xA5)
   * All arithmetic is done on 4-byte signed integers (little-endian).
   * Overflow results in script failure.
   */
  OP_1ADD = 0x8b,
  OP_1SUB = 0x8c,
  OP_2MUL = 0x8d, /* DISABLED */
  OP_2DIV = 0x8e, /* DISABLED */
  OP_NEGATE = 0x8f,
  OP_ABS = 0x90,
  OP_NOT = 0x91,
  OP_0NOTEQUAL = 0x92,
  OP_ADD = 0x93,
  OP_SUB = 0x94,
  OP_MUL = 0x95,    /* DISABLED */
  OP_DIV = 0x96,    /* DISABLED */
  OP_MOD = 0x97,    /* DISABLED */
  OP_LSHIFT = 0x98, /* DISABLED */
  OP_RSHIFT = 0x99, /* DISABLED */
  OP_BOOLAND = 0x9a,
  OP_BOOLOR = 0x9b,
  OP_NUMEQUAL = 0x9c,
  OP_NUMEQUALVERIFY = 0x9d,
  OP_NUMNOTEQUAL = 0x9e,
  OP_LESSTHAN = 0x9f,
  OP_GREATERTHAN = 0xa0,
  OP_LESSTHANOREQUAL = 0xa1,
  OP_GREATERTHANOREQUAL = 0xa2,
  OP_MIN = 0xa3,
  OP_MAX = 0xa4,
  OP_WITHIN = 0xa5,

  /*
   * Cryptographic opcodes (0xA6 - 0xAF)
   */
  OP_RIPEMD160 = 0xa6,
  OP_SHA1 = 0xa7,
  OP_SHA256 = 0xa8,
  OP_HASH160 = 0xa9, /* RIPEMD160(SHA256(x)) */
  OP_HASH256 = 0xaa, /* SHA256(SHA256(x)) */
  OP_CODESEPARATOR = 0xab,
  OP_CHECKSIG = 0xac,
  OP_CHECKSIGVERIFY = 0xad,
  OP_CHECKMULTISIG = 0xae,
  OP_CHECKMULTISIGVERIFY = 0xaf,

  /*
   * Reserved/expansion opcodes (0xB0 - 0xB9)
   * NOP1-10 reserved for future soft-fork upgrades.
   * Some have been repurposed by BIPs.
   */
  OP_NOP1 = 0xb0,
  OP_CHECKLOCKTIMEVERIFY = 0xb1, /* BIP-65: OP_NOP2 repurposed */
  OP_NOP2 = 0xb1,                /* Alias (pre-BIP-65) */
  OP_CHECKSEQUENCEVERIFY = 0xb2, /* BIP-112: OP_NOP3 repurposed */
  OP_NOP3 = 0xb2,                /* Alias (pre-BIP-112) */
  OP_NOP4 = 0xb3,
  OP_NOP5 = 0xb4,
  OP_NOP6 = 0xb5,
  OP_NOP7 = 0xb6,
  OP_NOP8 = 0xb7,
  OP_NOP9 = 0xb8,
  OP_NOP10 = 0xb9,

  /*
   * Tapscript opcodes (BIP-342)
   */
  OP_CHECKSIGADD = 0xba, /* BIP-342: For batch signature verification */

  /*
   * Reserved for internal use / invalid opcodes (0xBB - 0xFF)
   * OP_INVALIDOPCODE is used as a sentinel value.
   */
  OP_INVALIDOPCODE = 0xff

} script_opcode_t;

/*
 * Script type enumeration.
 * Identifies the pattern of a scriptPubKey for special handling.
 */
typedef enum {
  SCRIPT_TYPE_UNKNOWN = 0, /* Non-standard or unrecognized */
  SCRIPT_TYPE_P2PK = 1,    /* Pay to Public Key */
  SCRIPT_TYPE_P2PKH = 2,   /* Pay to Public Key Hash */
  SCRIPT_TYPE_P2SH = 3,    /* Pay to Script Hash (BIP-16) */
  SCRIPT_TYPE_P2WPKH = 4,  /* Pay to Witness Public Key Hash (SegWit v0) */
  SCRIPT_TYPE_P2WSH = 5,   /* Pay to Witness Script Hash (SegWit v0) */
  SCRIPT_TYPE_P2TR = 6,    /* Pay to Taproot (SegWit v1, BIP-341) */
  SCRIPT_TYPE_WITNESS_UNKNOWN = 7, /* Unknown witness version (future) */
  SCRIPT_TYPE_MULTISIG = 8,        /* Bare multisig */
  SCRIPT_TYPE_NULL_DATA = 9,       /* OP_RETURN data carrier */
} script_type_t;

/*
 * Witness version for SegWit outputs.
 */
#define WITNESS_VERSION_0 0    /* BIP-141: P2WPKH, P2WSH */
#define WITNESS_VERSION_1 1    /* BIP-341: P2TR (Taproot) */
#define WITNESS_VERSION_MAX 16 /* Maximum witness version */

/*
 * Script structure.
 * Represents a Bitcoin script as raw bytes.
 * The script does not own its data — caller must manage memory.
 */
typedef struct {
  const uint8_t *data; /* Script bytes */
  size_t len;          /* Script length */
} script_t;

/*
 * Mutable script structure.
 * Owns its data, must be freed with script_free().
 */
typedef struct {
  uint8_t *data;   /* Script bytes (owned) */
  size_t len;      /* Script length */
  size_t capacity; /* Allocated capacity */
} script_mut_t;

/*
 * Parsed opcode with its data.
 * Used when iterating through a script.
 */
typedef struct {
  script_opcode_t op;  /* The opcode */
  const uint8_t *data; /* Push data (NULL if not a push op) */
  size_t len;          /* Length of push data */
} script_op_t;

/*
 * Script iterator for parsing opcodes.
 */
typedef struct {
  const uint8_t *script; /* Script being iterated */
  size_t script_len;
  size_t pos;        /* Current position */
  echo_bool_t error; /* Set if parse error occurred */
} script_iter_t;

/*
 * Witness program structure.
 * A witness program is a scriptPubKey of the form: <version> <program>
 */
typedef struct {
  int version;         /* Witness version (0-16) */
  uint8_t program[40]; /* Witness program (20 or 32 bytes typically) */
  size_t program_len;  /* Length of program */
} witness_program_t;

/*
 * ============================================================================
 * SCRIPT STACK MACHINE
 * ============================================================================
 *
 * Bitcoin Script uses a stack-based virtual machine. Elements on the stack
 * are byte arrays that can be interpreted as numbers when needed.
 *
 * Script number encoding:
 *   - Little-endian byte order
 *   - Minimal encoding required (no unnecessary leading zeros)
 *   - Sign bit is the high bit of the last byte
 *   - Empty array represents zero
 *   - Maximum 4 bytes for arithmetic operations (but can store more)
 */

/*
 * Maximum size for script numbers in arithmetic operations.
 * BIP-62 rule 4: Script numbers must be at most 4 bytes.
 */
#define SCRIPT_NUM_MAX_SIZE 4

/*
 * Script number type.
 * 64-bit to avoid overflow during arithmetic, but values are
 * limited to 32-bit signed range for consensus.
 */
typedef int64_t script_num_t;

/*
 * Stack element.
 * Each element is a byte array with its own length.
 * Elements own their data and must be freed.
 */
typedef struct {
  uint8_t *data; /* Element bytes (owned, NULL for empty) */
  size_t len;    /* Length in bytes */
} stack_element_t;

/*
 * Script execution stack.
 * Fixed-capacity stack with SCRIPT_MAX_STACK_SIZE elements.
 */
typedef struct {
  stack_element_t *elements; /* Array of elements (owned) */
  size_t count;              /* Current number of elements */
  size_t capacity;           /* Allocated capacity */
} script_stack_t;

/*
 * Script execution flags.
 * Control various validation rules based on soft fork activation.
 */
typedef enum {
  SCRIPT_VERIFY_NONE = 0,
  SCRIPT_VERIFY_P2SH = (1 << 0),        /* BIP-16 */
  SCRIPT_VERIFY_STRICTENC = (1 << 1),   /* Require strict signature encoding */
  SCRIPT_VERIFY_DERSIG = (1 << 2),      /* BIP-66: Strict DER signatures */
  SCRIPT_VERIFY_LOW_S = (1 << 3),       /* BIP-62 rule 5: Low S values */
  SCRIPT_VERIFY_NULLDUMMY = (1 << 4),   /* BIP-62 rule 7: Dummy must be empty */
  SCRIPT_VERIFY_SIGPUSHONLY = (1 << 5), /* BIP-62 rule 2: scriptSig push-only */
  SCRIPT_VERIFY_MINIMALDATA =
      (1 << 6), /* BIP-62 rule 3/4: Minimal pushes/numbers */
  SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS = (1 << 7),
  SCRIPT_VERIFY_CLEANSTACK = (1 << 8), /* BIP-62 rule 6: Stack must be clean */
  SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY = (1 << 9),  /* BIP-65 */
  SCRIPT_VERIFY_CHECKSEQUENCEVERIFY = (1 << 10), /* BIP-112 */
  SCRIPT_VERIFY_WITNESS = (1 << 11),             /* BIP-141 */
  SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM = (1 << 12),
  SCRIPT_VERIFY_MINIMALIF = (1 << 13), /* BIP-141: Minimal IF/NOTIF args */
  SCRIPT_VERIFY_NULLFAIL = (1 << 14),  /* BIP-146: Empty sig on failure */
  SCRIPT_VERIFY_WITNESS_PUBKEYTYPE =
      (1 << 15), /* BIP-141: Compressed keys only */
  SCRIPT_VERIFY_CONST_SCRIPTCODE =
      (1 << 16),                     /* Making OP_CODESEPARATOR obsolete */
  SCRIPT_VERIFY_TAPROOT = (1 << 17), /* BIP-341 */
} script_verify_flags_t;

/*
 * Standard verification flags for current consensus.
 */
#define SCRIPT_VERIFY_STANDARD                                                 \
  (SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_DERSIG | SCRIPT_VERIFY_STRICTENC |       \
   SCRIPT_VERIFY_MINIMALDATA | SCRIPT_VERIFY_NULLDUMMY |                       \
   SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS | SCRIPT_VERIFY_CLEANSTACK |       \
   SCRIPT_VERIFY_MINIMALIF | SCRIPT_VERIFY_NULLFAIL |                          \
   SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY | SCRIPT_VERIFY_CHECKSEQUENCEVERIFY |     \
   SCRIPT_VERIFY_LOW_S | SCRIPT_VERIFY_WITNESS |                               \
   SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM |                       \
   SCRIPT_VERIFY_WITNESS_PUBKEYTYPE | SCRIPT_VERIFY_TAPROOT)

/*
 * Script execution error codes.
 */
typedef enum {
  SCRIPT_ERR_OK = 0,
  SCRIPT_ERR_UNKNOWN_ERROR,
  SCRIPT_ERR_EVAL_FALSE,
  SCRIPT_ERR_OP_RETURN,

  /* Numeric errors */
  SCRIPT_ERR_SCRIPT_SIZE,
  SCRIPT_ERR_PUSH_SIZE,
  SCRIPT_ERR_OP_COUNT,
  SCRIPT_ERR_STACK_SIZE,
  SCRIPT_ERR_SIG_COUNT,
  SCRIPT_ERR_PUBKEY_COUNT,

  /* Stack errors */
  SCRIPT_ERR_INVALID_STACK_OPERATION,
  SCRIPT_ERR_INVALID_ALTSTACK_OPERATION,
  SCRIPT_ERR_UNBALANCED_CONDITIONAL,

  /* Opcode errors */
  SCRIPT_ERR_DISABLED_OPCODE,
  SCRIPT_ERR_RESERVED_OPCODE,
  SCRIPT_ERR_BAD_OPCODE,
  SCRIPT_ERR_INVALID_OPCODE,

  /* Verify errors */
  SCRIPT_ERR_VERIFY,
  SCRIPT_ERR_EQUALVERIFY,
  SCRIPT_ERR_CHECKMULTISIGVERIFY,
  SCRIPT_ERR_CHECKSIGVERIFY,
  SCRIPT_ERR_NUMEQUALVERIFY,

  /* Number errors */
  SCRIPT_ERR_INVALID_NUMBER_RANGE,
  SCRIPT_ERR_IMPOSSIBLE_ENCODING,
  SCRIPT_ERR_NEGATIVE_LOCKTIME,
  SCRIPT_ERR_UNSATISFIED_LOCKTIME,
  SCRIPT_ERR_MINIMALDATA,

  /* Signature errors */
  SCRIPT_ERR_SIG_HASHTYPE,
  SCRIPT_ERR_SIG_DER,
  SCRIPT_ERR_SIG_HIGH_S,
  SCRIPT_ERR_SIG_NULLDUMMY,
  SCRIPT_ERR_SIG_NULLFAIL,
  SCRIPT_ERR_PUBKEYTYPE,
  SCRIPT_ERR_SIG_BADLENGTH,
  SCRIPT_ERR_SCHNORR_SIG,
  SCRIPT_ERR_SIG_PUSHONLY,

  /* SegWit errors */
  SCRIPT_ERR_WITNESS_PROGRAM_WRONG_LENGTH,
  SCRIPT_ERR_WITNESS_PROGRAM_WITNESS_EMPTY,
  SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH,
  SCRIPT_ERR_WITNESS_MALLEATED,
  SCRIPT_ERR_WITNESS_MALLEATED_P2SH,
  SCRIPT_ERR_WITNESS_UNEXPECTED,
  SCRIPT_ERR_WITNESS_PUBKEYTYPE,
  SCRIPT_ERR_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM,

  /* Taproot errors */
  SCRIPT_ERR_TAPROOT_WRONG_CONTROL_SIZE,
  SCRIPT_ERR_TAPSCRIPT_VALIDATION_WEIGHT,
  SCRIPT_ERR_TAPSCRIPT_CHECKMULTISIG,
  SCRIPT_ERR_TAPSCRIPT_MINIMALIF,

  /* Other evaluation errors */
  SCRIPT_ERR_CLEANSTACK,

  /* Memory errors */
  SCRIPT_ERR_OUT_OF_MEMORY,

  SCRIPT_ERR_ERROR_COUNT /* Number of error codes */
} script_error_t;

/*
 * Script interpreter context.
 * Maintains state during script execution.
 */
typedef struct {
  script_stack_t stack;    /* Main execution stack */
  script_stack_t altstack; /* Alternate stack (OP_TOALTSTACK/OP_FROMALTSTACK) */
  uint32_t flags;          /* Verification flags (script_verify_flags_t) */
  script_error_t error;    /* Last error code */
  size_t op_count;         /* Number of non-push opcodes executed */

  /*
   * Conditional execution state.
   * Bitcoin Script has IF/ELSE/ENDIF flow control.
   * We track nested conditions with a simple counter approach:
   *   - exec_depth: depth of IF blocks we're executing
   *   - skip_depth: depth of IF blocks we're skipping
   */
  int exec_depth; /* Depth of executed branches */
  int skip_depth; /* Depth of skipped branches (0 = executing) */

  /*
   * Transaction context for signature verification.
   * These fields must be set for CHECKSIG/CHECKMULTISIG to work.
   * If tx is NULL, signature verification always fails.
   */
  const tx_t *tx;             /* Spending transaction */
  size_t input_index;         /* Index of input being verified */
  satoshi_t amount;           /* Value of the UTXO being spent */
  const uint8_t *script_code; /* Script being signed (for sighash) */
  size_t script_code_len;     /* Length of script_code */
  size_t codesep_pos;         /* Position after last OP_CODESEPARATOR */

  /*
   * Currently executing script tracking.
   * Needed for proper OP_CODESEPARATOR handling when sig ops are in scriptSig.
   */
  const uint8_t *exec_script;  /* Currently executing script */
  size_t exec_script_len;      /* Length of currently executing script */
  size_t exec_script_pos;      /* Current position in executing script */

  /*
   * Signature hash cache for BIP-143 (SegWit v0).
   * These are precomputed once per transaction for efficiency.
   */
  echo_bool_t sighash_cached; /* True if cache is valid */
  uint8_t hash_prevouts[32];
  uint8_t hash_sequence[32];
  uint8_t hash_outputs[32];

  /*
   * Taproot execution context (BIP-341/342).
   * These fields are set during Taproot script path spending.
   */
  echo_bool_t is_tapscript;     /* True if executing Tapscript */
  uint8_t tapleaf_hash[32];     /* Leaf hash for script path spending */
  uint8_t tap_internal_key[32]; /* Internal key (x-only) */
  uint8_t key_version;          /* Key version (0 for BIP-342) */

  /*
   * Taproot sighash cache (BIP-341).
   * These are precomputed once per transaction for efficiency.
   */
  echo_bool_t tap_sighash_cached; /* True if Taproot cache is valid */
  uint8_t hash_amounts[32];       /* SHA256 of all input amounts */
  uint8_t hash_scriptpubkeys[32]; /* SHA256 of all scriptPubKeys */
} script_context_t;

/*
 * ============================================================================
 * STACK FUNCTIONS
 * ============================================================================
 */

/*
 * Initialize a script stack.
 *
 * Parameters:
 *   stack - Stack to initialize
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t stack_init(script_stack_t *stack);

/*
 * Free all memory owned by a stack.
 *
 * Parameters:
 *   stack - Stack to free (structure itself not freed)
 */
void stack_free(script_stack_t *stack);

/*
 * Clear all elements from a stack without freeing the stack itself.
 *
 * Parameters:
 *   stack - Stack to clear
 */
void stack_clear(script_stack_t *stack);

/*
 * Get the number of elements on the stack.
 *
 * Parameters:
 *   stack - Stack to query
 *
 * Returns:
 *   Number of elements
 */
size_t stack_size(const script_stack_t *stack);

/*
 * Check if stack is empty.
 *
 * Parameters:
 *   stack - Stack to check
 *
 * Returns:
 *   ECHO_TRUE if empty, ECHO_FALSE otherwise
 */
echo_bool_t stack_empty(const script_stack_t *stack);

/*
 * Push a byte array onto the stack.
 * The stack takes ownership of a copy of the data.
 *
 * Parameters:
 *   stack - Stack to push onto
 *   data  - Data to push (may be NULL if len is 0)
 *   len   - Length of data
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_OUT_OF_MEMORY on allocation failure
 *   ECHO_ERR_SCRIPT_STACK if stack would exceed limit
 */
echo_result_t stack_push(script_stack_t *stack, const uint8_t *data,
                         size_t len);

/*
 * Push a script number onto the stack.
 *
 * Parameters:
 *   stack - Stack to push onto
 *   num   - Number to push
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t stack_push_num(script_stack_t *stack, script_num_t num);

/*
 * Push a boolean onto the stack.
 * True is represented as {0x01}, false as {} (empty).
 *
 * Parameters:
 *   stack - Stack to push onto
 *   val   - Boolean value
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t stack_push_bool(script_stack_t *stack, echo_bool_t val);

/*
 * Pop the top element from the stack.
 * The caller takes ownership of the element's data.
 *
 * Parameters:
 *   stack - Stack to pop from
 *   elem  - Output: popped element (caller must free data)
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_STACK if stack is empty
 */
echo_result_t stack_pop(script_stack_t *stack, stack_element_t *elem);

/*
 * Pop the top element and interpret it as a number.
 *
 * Parameters:
 *   stack        - Stack to pop from
 *   num          - Output: the number value
 *   require_minimal - If true, require minimal encoding (BIP-62 rule 4)
 *   max_size     - Maximum allowed byte size (usually SCRIPT_NUM_MAX_SIZE)
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_STACK if stack is empty
 *   ECHO_ERR_INVALID_FORMAT if not valid number encoding
 */
echo_result_t stack_pop_num(script_stack_t *stack, script_num_t *num,
                            echo_bool_t require_minimal, size_t max_size);

/*
 * Pop the top element and interpret it as a boolean.
 *
 * Parameters:
 *   stack - Stack to pop from
 *   val   - Output: boolean value (true if non-zero)
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_STACK if stack is empty
 */
echo_result_t stack_pop_bool(script_stack_t *stack, echo_bool_t *val);

/*
 * Peek at the top element without removing it.
 *
 * Parameters:
 *   stack - Stack to peek
 *   elem  - Output: pointer to element (do not free, do not modify)
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_STACK if stack is empty
 */
echo_result_t stack_peek(const script_stack_t *stack,
                         const stack_element_t **elem);

/*
 * Peek at an element by index from top (0 = top, 1 = second from top, etc.)
 *
 * Parameters:
 *   stack - Stack to peek
 *   index - Index from top
 *   elem  - Output: pointer to element (do not free)
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_OUT_OF_RANGE if index >= stack size
 */
echo_result_t stack_peek_at(const script_stack_t *stack, size_t index,
                            const stack_element_t **elem);

/*
 * Duplicate the top element.
 * Implements OP_DUP.
 *
 * Parameters:
 *   stack - Stack to operate on
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t stack_dup(script_stack_t *stack);

/*
 * Remove the top element without returning it.
 * Implements OP_DROP.
 *
 * Parameters:
 *   stack - Stack to operate on
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_STACK if stack is empty
 */
echo_result_t stack_drop(script_stack_t *stack);

/*
 * Swap the top two elements.
 * Implements OP_SWAP.
 *
 * Parameters:
 *   stack - Stack to operate on
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_STACK if stack has fewer than 2 elements
 */
echo_result_t stack_swap(script_stack_t *stack);

/*
 * Rotate the top three elements: (x1 x2 x3 -- x2 x3 x1)
 * Implements OP_ROT.
 *
 * Parameters:
 *   stack - Stack to operate on
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_STACK if stack has fewer than 3 elements
 */
echo_result_t stack_rot(script_stack_t *stack);

/*
 * Copy the second-to-top element to the top.
 * Implements OP_OVER.
 *
 * Parameters:
 *   stack - Stack to operate on
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_STACK if stack has fewer than 2 elements
 */
echo_result_t stack_over(script_stack_t *stack);

/*
 * Remove the second-to-top element.
 * Implements OP_NIP.
 *
 * Parameters:
 *   stack - Stack to operate on
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_STACK if stack has fewer than 2 elements
 */
echo_result_t stack_nip(script_stack_t *stack);

/*
 * Copy the top element and insert it below the second element.
 * Implements OP_TUCK: (x1 x2 -- x2 x1 x2)
 *
 * Parameters:
 *   stack - Stack to operate on
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_STACK if stack has fewer than 2 elements
 */
echo_result_t stack_tuck(script_stack_t *stack);

/*
 * Duplicate top two elements.
 * Implements OP_2DUP: (x1 x2 -- x1 x2 x1 x2)
 *
 * Parameters:
 *   stack - Stack to operate on
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t stack_2dup(script_stack_t *stack);

/*
 * Duplicate top three elements.
 * Implements OP_3DUP: (x1 x2 x3 -- x1 x2 x3 x1 x2 x3)
 *
 * Parameters:
 *   stack - Stack to operate on
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t stack_3dup(script_stack_t *stack);

/*
 * Drop top two elements.
 * Implements OP_2DROP: (x1 x2 -- )
 *
 * Parameters:
 *   stack - Stack to operate on
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t stack_2drop(script_stack_t *stack);

/*
 * Copy elements 3 and 4 to top.
 * Implements OP_2OVER: (x1 x2 x3 x4 -- x1 x2 x3 x4 x1 x2)
 *
 * Parameters:
 *   stack - Stack to operate on
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t stack_2over(script_stack_t *stack);

/*
 * Swap top two pairs.
 * Implements OP_2SWAP: (x1 x2 x3 x4 -- x3 x4 x1 x2)
 *
 * Parameters:
 *   stack - Stack to operate on
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t stack_2swap(script_stack_t *stack);

/*
 * Rotate top three pairs.
 * Implements OP_2ROT: (x1 x2 x3 x4 x5 x6 -- x3 x4 x5 x6 x1 x2)
 *
 * Parameters:
 *   stack - Stack to operate on
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t stack_2rot(script_stack_t *stack);

/*
 * Copy nth element to top.
 * Implements OP_PICK.
 *
 * Parameters:
 *   stack - Stack to operate on
 *   n     - Index from top (0 = top element)
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_OUT_OF_RANGE if n >= stack size
 */
echo_result_t stack_pick(script_stack_t *stack, size_t n);

/*
 * Move nth element to top.
 * Implements OP_ROLL.
 *
 * Parameters:
 *   stack - Stack to operate on
 *   n     - Index from top (0 = no-op)
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_OUT_OF_RANGE if n >= stack size
 */
echo_result_t stack_roll(script_stack_t *stack, size_t n);

/*
 * ============================================================================
 * NUMBER CONVERSION FUNCTIONS
 * ============================================================================
 */

/*
 * Convert a byte array to a script number.
 *
 * Bitcoin script numbers use:
 *   - Little-endian byte order
 *   - Sign-magnitude representation (high bit of last byte is sign)
 *   - Minimal encoding (no unnecessary leading zeros)
 *
 * Parameters:
 *   data            - Byte array to convert
 *   len             - Length of byte array
 *   num             - Output: the number value
 *   require_minimal - If true, reject non-minimal encodings
 *   max_size        - Maximum allowed byte size
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_INVALID_FORMAT if encoding invalid
 *   ECHO_ERR_OUT_OF_RANGE if exceeds max_size
 */
echo_result_t script_num_decode(const uint8_t *data, size_t len,
                                script_num_t *num, echo_bool_t require_minimal,
                                size_t max_size);

/*
 * Convert a script number to minimal byte array.
 *
 * Parameters:
 *   num  - Number to convert
 *   buf  - Output buffer (must be at least 9 bytes for safety)
 *   len  - Output: number of bytes written
 *
 * Returns:
 *   ECHO_OK on success
 */
echo_result_t script_num_encode(script_num_t num, uint8_t *buf, size_t *len);

/*
 * Check if a byte array represents "true" (non-zero) in script.
 *
 * Script boolean semantics:
 *   - Empty array is false
 *   - Array of all zeros is false
 *   - Special case: negative zero (0x80) is false
 *   - Everything else is true
 *
 * Parameters:
 *   data - Byte array to check
 *   len  - Length of byte array
 *
 * Returns:
 *   ECHO_TRUE if data represents true, ECHO_FALSE otherwise
 */
echo_bool_t script_bool(const uint8_t *data, size_t len);

/*
 * ============================================================================
 * CONTEXT FUNCTIONS
 * ============================================================================
 */

/*
 * Initialize a script execution context.
 *
 * Parameters:
 *   ctx   - Context to initialize
 *   flags - Verification flags
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t script_context_init(script_context_t *ctx, uint32_t flags);

/*
 * Free all resources owned by a context.
 *
 * Parameters:
 *   ctx - Context to free (structure itself not freed)
 */
void script_context_free(script_context_t *ctx);

/*
 * Get error message for a script error code.
 *
 * Parameters:
 *   err - Error code
 *
 * Returns:
 *   Static string describing the error
 */
const char *script_error_string(script_error_t err);

/*
 * ============================================================================
 * SCRIPT BUILDER FUNCTIONS
 * ============================================================================
 */

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
size_t script_sigops_count(const uint8_t *data, size_t len,
                           echo_bool_t accurate);

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

/*
 * ============================================================================
 * OPCODE EXECUTION
 * ============================================================================
 */

/*
 * Execute a single opcode.
 *
 * Handles all standard opcodes:
 *   - Push: OP_0, OP_1NEGATE, OP_1-OP_16, direct pushes
 *   - Flow: OP_NOP, OP_IF, OP_NOTIF, OP_ELSE, OP_ENDIF, OP_VERIFY, OP_RETURN
 *   - Stack: OP_TOALTSTACK, OP_FROMALTSTACK, OP_DEPTH, OP_DROP, OP_DUP, etc.
 *   - Splice: OP_SIZE
 *   - Comparison: OP_EQUAL, OP_EQUALVERIFY
 *   - Arithmetic: OP_1ADD, OP_1SUB, OP_NEGATE, OP_ABS, OP_NOT, OP_0NOTEQUAL,
 *                 OP_ADD, OP_SUB, OP_BOOLAND, OP_BOOLOR, OP_NUMEQUAL, etc.
 *   - Crypto: CHECKSIG, CHECKMULTISIG, hash operations
 *
 * Parameters:
 *   ctx      - Script execution context
 *   op       - Opcode with any push data
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_* on script failure (sets ctx->error)
 */
echo_result_t script_exec_op(script_context_t *ctx, const script_op_t *op);

/*
 * Execute a complete script.
 *
 * Iterates through all opcodes in the script and executes them.
 * Stops on first error or OP_RETURN.
 *
 * Parameters:
 *   ctx   - Script execution context (must be initialized)
 *   data  - Script bytes
 *   len   - Script length
 *
 * Returns:
 *   ECHO_OK if script executed successfully
 *   Error code on failure (ctx->error has script-specific error)
 */
echo_result_t script_execute(script_context_t *ctx, const uint8_t *data,
                             size_t len);

/*
 * Check if currently executing (not skipping due to IF/ELSE).
 *
 * Parameters:
 *   ctx - Script context
 *
 * Returns:
 *   ECHO_TRUE if executing, ECHO_FALSE if skipping
 */
echo_bool_t script_is_executing(const script_context_t *ctx);

/*
 * ============================================================================
 * TRANSACTION CONTEXT AND SIGHASH
 * ============================================================================
 */

/*
 * Set transaction context for signature verification.
 *
 * This must be called before executing scripts that contain CHECKSIG
 * or CHECKMULTISIG opcodes for actual signature verification.
 *
 * Parameters:
 *   ctx         - Script context
 *   tx          - Spending transaction
 *   input_index - Index of the input being verified
 *   amount      - Value of the UTXO being spent (for BIP-143)
 *   script_code - The script being signed
 *   script_code_len - Length of script_code
 */
void script_set_tx_context(script_context_t *ctx, const tx_t *tx,
                           size_t input_index, satoshi_t amount,
                           const uint8_t *script_code, size_t script_code_len);

/*
 * Compute BIP-143 signature hash for SegWit v0.
 *
 * This implements the signature hash algorithm defined in BIP-143,
 * which is used for P2WPKH, P2WSH, and P2SH-wrapped witness scripts.
 *
 * Parameters:
 *   ctx           - Script context with transaction set
 *   sighash_type  - Sighash type (SIGHASH_ALL, SIGHASH_NONE, etc.)
 *   sighash       - Output: 32-byte signature hash
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if parameters are invalid
 */
echo_result_t sighash_bip143(script_context_t *ctx, uint32_t sighash_type,
                             uint8_t sighash[32]);

/*
 * Compute legacy signature hash (pre-SegWit).
 *
 * This implements the original Bitcoin signature hash algorithm.
 *
 * Parameters:
 *   ctx           - Script context with transaction set
 *   sighash_type  - Sighash type
 *   sighash       - Output: 32-byte signature hash
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if parameters are invalid
 */
echo_result_t sighash_legacy(script_context_t *ctx, uint32_t sighash_type,
                             uint8_t sighash[32]);

/*
 * Verify a P2WPKH (Pay to Witness Public Key Hash) script.
 *
 * Witness: [signature, pubkey]
 * Script: OP_0 <20-byte-hash>
 *
 * Parameters:
 *   ctx        - Script context with transaction set
 *   witness    - Witness stack (2 items: sig, pubkey)
 *   pubkey_hash - 20-byte pubkey hash from scriptPubKey
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_* on verification failure
 */
echo_result_t script_verify_p2wpkh(script_context_t *ctx,
                                   const uint8_t *witness_data,
                                   size_t witness_len,
                                   const uint8_t pubkey_hash[20]);

/*
 * Verify a P2WSH (Pay to Witness Script Hash) script.
 *
 * Witness: [input1, input2, ..., witnessScript]
 * Script: OP_0 <32-byte-hash>
 *
 * Parameters:
 *   ctx          - Script context with transaction set
 *   witness_data - Serialized witness stack
 *   witness_len  - Length of witness data
 *   script_hash  - 32-byte script hash from scriptPubKey
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_* on verification failure
 */
echo_result_t script_verify_p2wsh(script_context_t *ctx,
                                  const uint8_t *witness_data,
                                  size_t witness_len,
                                  const uint8_t script_hash[32]);

/*
 * ============================================================================
 * TAPROOT SCRIPT EXECUTION (BIP-341/342)
 * ============================================================================
 */

/*
 * Compute BIP-341 signature hash for Taproot.
 *
 * This implements the Taproot-specific signature hash algorithm.
 * Unlike BIP-143, it uses tagged hashes and commits to additional data.
 *
 * Parameters:
 *   ctx           - Script context with transaction set
 *   sighash_type  - Sighash type (0x00 for default, or standard types)
 *   ext_flag      - Extension flag (0 for key path, 1 for script path)
 *   sighash       - Output: 32-byte signature hash
 *
 * For script path spending (ext_flag=1), the tapleaf_hash and codesep_pos
 * fields in ctx must be set.
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if parameters are invalid
 */
echo_result_t sighash_taproot(script_context_t *ctx, uint32_t sighash_type,
                              uint8_t ext_flag, uint8_t sighash[32]);

/*
 * Verify a Taproot key path spend.
 *
 * Key path spending is the simplest Taproot spending path:
 * just verify a Schnorr signature against the output key.
 *
 * The witness contains: [signature]
 * Where signature is 64 bytes (or 65 bytes with sighash type appended).
 *
 * Parameters:
 *   ctx         - Script context with transaction set
 *   witness_data - Serialized witness stack
 *   witness_len  - Length of witness data
 *   output_key   - 32-byte x-only output key from scriptPubKey
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_* on verification failure
 */
echo_result_t script_verify_taproot_keypath(script_context_t *ctx,
                                            const uint8_t *witness_data,
                                            size_t witness_len,
                                            const uint8_t output_key[32]);

/*
 * Parse a Taproot control block.
 *
 * The control block contains:
 *   - 1 byte: leaf_version | output_key_parity
 *   - 32 bytes: internal key (x-only)
 *   - 0+ * 32 bytes: Merkle proof nodes
 *
 * Parameters:
 *   control      - Control block data
 *   control_len  - Length of control block
 *   leaf_version - Output: leaf version (upper 7 bits)
 *   parity       - Output: output key parity (bit 0)
 *   internal_key - Output: 32-byte internal key
 *   merkle_path  - Output: pointer to start of Merkle path
 *   path_len     - Output: number of nodes in path
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_INVALID_FORMAT if control block is malformed
 */
echo_result_t
taproot_parse_control_block(const uint8_t *control, size_t control_len,
                            uint8_t *leaf_version, uint8_t *parity,
                            uint8_t internal_key[32],
                            const uint8_t **merkle_path, size_t *path_len);

/*
 * Compute the Taproot leaf hash.
 *
 * leaf_hash = tagged_hash("TapLeaf", leaf_version || compact_size(script_len)
 * || script)
 *
 * Parameters:
 *   leaf_version - Leaf version byte
 *   script       - Script data
 *   script_len   - Script length
 *   leaf_hash    - Output: 32-byte leaf hash
 *
 * Returns:
 *   ECHO_OK on success
 */
echo_result_t taproot_leaf_hash(uint8_t leaf_version, const uint8_t *script,
                                size_t script_len, uint8_t leaf_hash[32]);

/*
 * Verify the Taproot Merkle proof.
 *
 * Computes the Merkle root from the leaf hash and proof path,
 * then verifies that tweaking the internal key produces the output key.
 *
 * Parameters:
 *   leaf_hash     - 32-byte leaf hash
 *   merkle_path   - Array of 32-byte nodes
 *   path_len      - Number of nodes in path
 *   internal_key  - 32-byte internal key
 *   output_key    - 32-byte expected output key
 *   parity        - Expected output key parity
 *
 * Returns:
 *   ECHO_OK if proof is valid
 *   ECHO_ERR_SCRIPT_ERROR if proof is invalid
 */
echo_result_t taproot_verify_merkle_proof(const uint8_t leaf_hash[32],
                                          const uint8_t *merkle_path,
                                          size_t path_len,
                                          const uint8_t internal_key[32],
                                          const uint8_t output_key[32],
                                          uint8_t parity);

/*
 * Verify a Taproot script path spend.
 *
 * Script path spending uses:
 * Witness: [stack items...] [script] [control block]
 *
 * Validation:
 * 1. Parse control block to get internal key and Merkle proof
 * 2. Compute leaf hash from script and leaf version
 * 3. Verify Merkle proof against output key
 * 4. Execute script (Tapscript rules if v0xC0)
 *
 * Parameters:
 *   ctx          - Script context with transaction set
 *   witness_data - Serialized witness stack
 *   witness_len  - Length of witness data
 *   output_key   - 32-byte x-only output key from scriptPubKey
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_* on verification failure
 */
echo_result_t script_verify_taproot_scriptpath(script_context_t *ctx,
                                               const uint8_t *witness_data,
                                               size_t witness_len,
                                               const uint8_t output_key[32]);

/*
 * Execute a Tapscript (BIP-342).
 *
 * Tapscript execution differs from legacy script:
 * - OP_CHECKMULTISIG and OP_CHECKMULTISIGVERIFY are disabled
 * - OP_CHECKSIGADD is enabled
 * - Success opcodes (OP_SUCCESSx) cause immediate success
 * - Signature validation uses BIP-340 Schnorr
 * - MINIMALIF is always enforced
 *
 * Parameters:
 *   ctx    - Script context (must have is_tapscript=true, tapleaf_hash set)
 *   script - Script to execute
 *   len    - Script length
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_* on failure
 */
echo_result_t script_execute_tapscript(script_context_t *ctx,
                                       const uint8_t *script, size_t len);

/*
 * ============================================================================
 * P2SH EVALUATION (BIP-16)
 * ============================================================================
 */

/*
 * Verify a P2SH (Pay to Script Hash) script.
 *
 * P2SH evaluation (BIP-16):
 * 1. Execute scriptSig — must leave serialized redeem script on stack
 * 2. Copy the stack for redeem script execution
 * 3. Verify HASH160(redeem_script) == script_hash
 * 4. Execute redeem script with remaining stack
 * 5. For P2SH-wrapped SegWit, delegate to SegWit verification
 *
 * The scriptSig must be push-only when SCRIPT_VERIFY_SIGPUSHONLY is set.
 *
 * Parameters:
 *   ctx            - Script context with transaction set
 *   script_sig     - The scriptSig (unlocking script)
 *   script_sig_len - Length of scriptSig
 *   script_hash    - 20-byte script hash from scriptPubKey
 *   witness_data   - Witness data (for P2SH-SegWit, may be NULL)
 *   witness_len    - Length of witness data
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_* on verification failure
 */
echo_result_t
script_verify_p2sh(script_context_t *ctx, const uint8_t *script_sig,
                   size_t script_sig_len, const uint8_t script_hash[20],
                   const uint8_t *witness_data, size_t witness_len);

/*
 * Check if a scriptSig is push-only.
 *
 * A push-only script contains only push opcodes (OP_0, OP_1-OP_16,
 * OP_1NEGATE, direct pushes 0x01-0x4b, OP_PUSHDATA1/2/4).
 *
 * BIP-16 requires scriptSig to be push-only for P2SH.
 *
 * Parameters:
 *   script - Script bytes
 *   len    - Script length
 *
 * Returns:
 *   ECHO_TRUE if push-only, ECHO_FALSE otherwise
 */
echo_bool_t script_is_push_only(const uint8_t *script, size_t len);

/*
 * Extract the serialized redeem script from an executed scriptSig.
 *
 * After executing a P2SH scriptSig, the top stack element contains
 * the serialized redeem script. This function extracts it.
 *
 * Parameters:
 *   ctx           - Script context after executing scriptSig
 *   redeem_script - Output: pointer to redeem script data
 *   redeem_len    - Output: length of redeem script
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_SCRIPT_STACK if stack is empty
 */
echo_result_t script_get_redeem_script(const script_context_t *ctx,
                                       const uint8_t **redeem_script,
                                       size_t *redeem_len);

#endif /* ECHO_SCRIPT_H */
