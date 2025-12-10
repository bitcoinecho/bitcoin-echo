/*
 * Bitcoin Echo — Fundamental Types
 *
 * This header defines the core types used throughout Bitcoin Echo.
 * All types are fixed-width for portability and protocol correctness.
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_TYPES_H
#define ECHO_TYPES_H

#include <stdint.h>
#include <stddef.h>

/*
 * Boolean type.
 * C11 provides _Bool and stdbool.h, but we define our own for clarity.
 */
typedef int echo_bool_t;
#define ECHO_TRUE  1
#define ECHO_FALSE 0

/*
 * Fixed-width integer types.
 * These come from stdint.h but we document their use here:
 *   uint8_t   - single byte (opcodes, flags)
 *   uint16_t  - port numbers, version fields
 *   uint32_t  - block height, timestamps, nonce, sequence
 *   uint64_t  - services flags, compact size integers
 *   int32_t   - version numbers (signed per protocol)
 *   int64_t   - satoshi amounts (signed to detect underflow)
 */

/*
 * Satoshi amount type.
 * Signed 64-bit to allow detection of underflow during validation.
 * Valid range: 0 to 21,000,000 * 100,000,000 (2.1 quadrillion satoshis).
 */
typedef int64_t satoshi_t;

/* Maximum possible satoshis (21 million BTC) */
#define ECHO_MAX_SATOSHIS ((satoshi_t)2100000000000000LL)

/* Satoshis per bitcoin */
#define ECHO_SATOSHIS_PER_BTC ((satoshi_t)100000000LL)

/*
 * Hash types.
 * Bitcoin uses two hash sizes: 256-bit (SHA256d) and 160-bit (HASH160).
 */

/* 256-bit hash (32 bytes) - used for block hashes, txids, merkle nodes */
typedef struct {
    uint8_t bytes[32];
} hash256_t;

/* 160-bit hash (20 bytes) - used for addresses (RIPEMD160(SHA256(x))) */
typedef struct {
    uint8_t bytes[20];
} hash160_t;

/* Null hash constant (32 zero bytes) - used for coinbase prevout */
#define ECHO_NULL_HASH256 ((hash256_t){{0}})

/*
 * Result codes.
 * Functions return these to indicate success or specific failure reasons.
 * Zero is success; negative values indicate errors.
 */
typedef enum {
    ECHO_OK = 0,

    /* General errors */
    ECHO_ERR_NULL_PARAM      = -1,   /* Required parameter was NULL */
    ECHO_ERR_BUFFER_TOO_SMALL = -2,  /* Output buffer insufficient */
    ECHO_ERR_OUT_OF_MEMORY   = -3,   /* Memory allocation failed */
    ECHO_ERR_OUT_OF_RANGE    = -4,   /* Index out of valid range */
    ECHO_ERR_NOT_FOUND       = -5,   /* Item not found */

    /* Parsing errors */
    ECHO_ERR_PARSE_FAILED    = -10,  /* General parse failure */
    ECHO_ERR_TRUNCATED       = -11,  /* Unexpected end of data */
    ECHO_ERR_INVALID_FORMAT  = -12,  /* Data format invalid */
    ECHO_ERR_VARINT_TOO_LARGE = -13, /* CompactSize exceeds maximum */

    /* Validation errors - transactions */
    ECHO_ERR_TX_EMPTY_INPUTS  = -20, /* Transaction has no inputs */
    ECHO_ERR_TX_EMPTY_OUTPUTS = -21, /* Transaction has no outputs */
    ECHO_ERR_TX_DUPLICATE_INPUT = -22, /* Same outpoint spent twice */
    ECHO_ERR_TX_NEGATIVE_VALUE = -23, /* Output value negative */
    ECHO_ERR_TX_VALUE_OVERFLOW = -24, /* Total output exceeds max */
    ECHO_ERR_TX_SCRIPT_FAILED = -25, /* Script execution failed */

    /* Validation errors - blocks */
    ECHO_ERR_BLOCK_POW_FAILED = -30, /* Proof-of-work invalid */
    ECHO_ERR_BLOCK_MERKLE     = -31, /* Merkle root mismatch */
    ECHO_ERR_BLOCK_TIMESTAMP  = -32, /* Timestamp out of range */
    ECHO_ERR_BLOCK_SIZE       = -33, /* Block exceeds size limit */
    ECHO_ERR_BLOCK_NO_COINBASE = -34, /* First tx not coinbase */
    ECHO_ERR_BLOCK_MULTI_COINBASE = -35, /* Multiple coinbase txs */

    /* Validation errors - scripts */
    ECHO_ERR_SCRIPT_SIZE      = -40, /* Script exceeds size limit */
    ECHO_ERR_SCRIPT_OPCODE    = -41, /* Invalid or disabled opcode */
    ECHO_ERR_SCRIPT_STACK     = -42, /* Stack overflow/underflow */
    ECHO_ERR_SCRIPT_VERIFY    = -43, /* OP_VERIFY failed */
    ECHO_ERR_SCRIPT_SIG       = -44, /* Signature verification failed */
    ECHO_ERR_SCRIPT_ERROR     = -45, /* General script error (check ctx->error) */

    /* Consensus errors */
    ECHO_ERR_CONSENSUS_UTXO   = -50, /* UTXO not found */
    ECHO_ERR_CONSENSUS_SPENT  = -51, /* UTXO already spent */
    ECHO_ERR_CONSENSUS_MATURITY = -52, /* Coinbase not mature */

    /* UTXO errors */
    ECHO_ERR_EXISTS           = -55, /* Item already exists */
    ECHO_ERR_NOMEM            = -56, /* Memory allocation failed */

    /* Chain state errors */
    ECHO_ERR_INVALID_BLOCK    = -60, /* Block invalid or doesn't connect */
    ECHO_ERR_UNDERFLOW        = -61, /* Arithmetic underflow */
    ECHO_ERR_OVERFLOW         = -62, /* Arithmetic overflow */

    /* Platform errors */
    ECHO_ERR_PLATFORM_IO      = -70, /* I/O operation failed */
    ECHO_ERR_PLATFORM_NET     = -71, /* Network operation failed */

} echo_result_t;

/*
 * Byte buffer with length.
 * Used for variable-length data like scripts and signatures.
 */
typedef struct {
    const uint8_t *data;
    size_t         len;
} echo_buffer_t;

/*
 * Mutable byte buffer for output.
 */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   capacity;
} echo_buffer_mut_t;

#endif /* ECHO_TYPES_H */
