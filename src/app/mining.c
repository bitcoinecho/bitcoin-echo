/*
 * Bitcoin Echo — Mining Support
 *
 * Implementation of mining-related functionality for regtest testing.
 *
 * Build once. Build right. Stop.
 */

#include "mining.h"
#include "block.h"
#include "echo_types.h"
#include "tx.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Genesis block merkle root (same for all networks - it's the coinbase txid).
 * This is the SHA256d of the genesis coinbase transaction.
 */
static const uint8_t GENESIS_MERKLE_ROOT[32] = {
    0x3b, 0xa3, 0xed, 0xfd, 0x7a, 0x7b, 0x12, 0xb2, 0x7a, 0xc7, 0x2c,
    0x3e, 0x67, 0x76, 0x8f, 0x61, 0x7f, 0xc8, 0x1b, 0xc3, 0x88, 0x8a,
    0x51, 0x32, 0x3a, 0x9f, 0xb8, 0xaa, 0x4b, 0x1e, 0x5e, 0x4a};

/*
 * ============================================================================
 * COINBASE PARAMETERS
 * ============================================================================
 */

void coinbase_params_init(coinbase_params_t *params) {
  if (params == NULL)
    return;

  params->height = 0;
  params->value = 0;
  params->output_script = NULL;
  params->output_script_len = 0;
  params->extra_data = NULL;
  params->extra_data_len = 0;
  params->include_witness_commitment = ECHO_FALSE;
  memset(&params->witness_commitment, 0, sizeof(hash256_t));
}

/*
 * ============================================================================
 * HEIGHT ENCODING
 * ============================================================================
 */

echo_result_t coinbase_encode_height(uint32_t height, uint8_t *buf,
                                     size_t buf_len, size_t *written) {
  if (buf == NULL || written == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /*
   * BIP-34 height encoding uses minimally-encoded script numbers.
   *
   * For height 0: just OP_0 (0x00) - but block 0 is genesis, never mined
   * For height 1-16: OP_1 through OP_16 (0x51-0x60)
   * For height > 16: push the height as little-endian bytes
   *
   * The encoding is:
   *   - Push opcode (length byte)
   *   - Height as little-endian bytes (minimal encoding)
   */

  if (height == 0) {
    /* OP_0 */
    if (buf_len < 1) {
      return ECHO_ERR_BUFFER_TOO_SMALL;
    }
    buf[0] = 0x00; /* OP_0 */
    *written = 1;
    return ECHO_OK;
  }

  /* Calculate how many bytes needed for the height */
  size_t num_bytes;
  uint32_t temp = height;

  if (height <= 0x7f) {
    /* 1 byte (value fits in 7 bits, high bit clear) */
    num_bytes = 1;
  } else if (height <= 0x7fff) {
    /* 2 bytes */
    num_bytes = 2;
  } else if (height <= 0x7fffff) {
    /* 3 bytes */
    num_bytes = 3;
  } else if (height <= 0x7fffffff) {
    /* 4 bytes */
    num_bytes = 4;
  } else {
    /* 5 bytes (shouldn't happen for valid heights) */
    num_bytes = 5;
  }

  /* Check for high bit - if set, need extra byte to keep positive */
  uint8_t high_byte;
  if (num_bytes == 1) {
    high_byte = (uint8_t)(height & 0xff);
  } else if (num_bytes == 2) {
    high_byte = (uint8_t)((height >> 8) & 0xff);
  } else if (num_bytes == 3) {
    high_byte = (uint8_t)((height >> 16) & 0xff);
  } else if (num_bytes == 4) {
    high_byte = (uint8_t)((height >> 24) & 0xff);
  } else {
    high_byte = 0;
  }

  if (high_byte & 0x80) {
    num_bytes++; /* Need extra byte for sign */
  }

  size_t total_len = 1 + num_bytes; /* push opcode + data */
  if (buf_len < total_len) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }

  /* Write push opcode (length) */
  buf[0] = (uint8_t)num_bytes;

  /* Write height as little-endian */
  temp = height;
  for (size_t i = 0; i < num_bytes; i++) {
    if (i < 4) {
      buf[1 + i] = (uint8_t)(temp & 0xff);
      temp >>= 8;
    } else {
      buf[1 + i] = 0; /* Sign extension byte */
    }
  }

  *written = total_len;
  return ECHO_OK;
}

/*
 * ============================================================================
 * COINBASE CREATION
 * ============================================================================
 */

echo_result_t coinbase_create(const coinbase_params_t *params, tx_t *tx) {
  if (params == NULL || tx == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  tx_init(tx);

  /* Encode height for scriptsig */
  uint8_t height_buf[9];
  size_t height_len;
  echo_result_t res =
      coinbase_encode_height(params->height, height_buf, sizeof(height_buf),
                             &height_len);
  if (res != ECHO_OK) {
    return res;
  }

  /* Calculate scriptsig length: height encoding + extra data */
  size_t scriptsig_len = height_len + params->extra_data_len;

  /* Calculate number of outputs */
  size_t output_count = 1; /* Main output */
  if (params->include_witness_commitment) {
    output_count = 2; /* Main output + witness commitment */
  }

  /* Allocate input */
  tx->inputs = calloc(1, sizeof(tx_input_t));
  if (tx->inputs == NULL) {
    return ECHO_ERR_OUT_OF_MEMORY;
  }
  tx->input_count = 1;

  /* Set up coinbase input */
  tx_input_t *input = &tx->inputs[0];

  /* Null outpoint (all zeros txid, vout = 0xffffffff) */
  memset(&input->prevout.txid, 0, sizeof(hash256_t));
  input->prevout.vout = 0xFFFFFFFF;

  /* Allocate and build scriptsig */
  input->script_sig = malloc(scriptsig_len);
  if (input->script_sig == NULL) {
    tx_free(tx);
    return ECHO_ERR_OUT_OF_MEMORY;
  }
  input->script_sig_len = scriptsig_len;

  /* Copy height encoding */
  memcpy(input->script_sig, height_buf, height_len);

  /* Copy extra data if present */
  if (params->extra_data != NULL && params->extra_data_len > 0) {
    memcpy(input->script_sig + height_len, params->extra_data,
           params->extra_data_len);
  }

  /* Sequence number (typically 0xffffffff for coinbase) */
  input->sequence = 0xFFFFFFFF;

  /* Initialize empty witness stack */
  input->witness.items = NULL;
  input->witness.count = 0;

  /* If including witness commitment, add witness nonce to coinbase */
  if (params->include_witness_commitment) {
    input->witness.items = calloc(1, sizeof(witness_item_t));
    if (input->witness.items == NULL) {
      tx_free(tx);
      return ECHO_ERR_OUT_OF_MEMORY;
    }
    input->witness.count = 1;

    /* Witness nonce is 32 zero bytes */
    input->witness.items[0].data = calloc(32, 1);
    if (input->witness.items[0].data == NULL) {
      tx_free(tx);
      return ECHO_ERR_OUT_OF_MEMORY;
    }
    input->witness.items[0].len = 32;
    tx->has_witness = ECHO_TRUE;
  }

  /* Allocate outputs */
  tx->outputs = calloc(output_count, sizeof(tx_output_t));
  if (tx->outputs == NULL) {
    tx_free(tx);
    return ECHO_ERR_OUT_OF_MEMORY;
  }
  tx->output_count = output_count;

  /* Set up main output */
  tx_output_t *main_output = &tx->outputs[0];
  main_output->value = params->value;

  if (params->output_script != NULL && params->output_script_len > 0) {
    /* Use provided script */
    main_output->script_pubkey = malloc(params->output_script_len);
    if (main_output->script_pubkey == NULL) {
      tx_free(tx);
      return ECHO_ERR_OUT_OF_MEMORY;
    }
    memcpy(main_output->script_pubkey, params->output_script,
           params->output_script_len);
    main_output->script_pubkey_len = params->output_script_len;
  } else {
    /* Default: OP_TRUE (0x51) - anyone can spend, for testing */
    main_output->script_pubkey = malloc(1);
    if (main_output->script_pubkey == NULL) {
      tx_free(tx);
      return ECHO_ERR_OUT_OF_MEMORY;
    }
    main_output->script_pubkey[0] = 0x51; /* OP_TRUE */
    main_output->script_pubkey_len = 1;
  }

  /* Set up witness commitment output if requested */
  if (params->include_witness_commitment) {
    tx_output_t *commit_output = &tx->outputs[1];
    commit_output->value = 0; /* Witness commitment has no value */

    /* Witness commitment script:
     * OP_RETURN (0x6a) + push 36 bytes + witness commitment header +
     * commitment */
    size_t commit_script_len = 1 + 1 + 4 + 32; /* OP_RETURN + push + header +
                                                  hash */
    commit_output->script_pubkey = malloc(commit_script_len);
    if (commit_output->script_pubkey == NULL) {
      tx_free(tx);
      return ECHO_ERR_OUT_OF_MEMORY;
    }
    commit_output->script_pubkey_len = commit_script_len;

    uint8_t *p = commit_output->script_pubkey;
    p[0] = 0x6a;                      /* OP_RETURN */
    p[1] = 36;                        /* Push 36 bytes */
    p[2] = 0xaa;                      /* Witness commitment header */
    p[3] = 0x21;
    p[4] = 0xa9;
    p[5] = 0xed;
    memcpy(p + 6, params->witness_commitment.bytes, 32);
  }

  /* Transaction version and locktime */
  tx->version = 1;
  tx->locktime = 0;

  return ECHO_OK;
}

/*
 * ============================================================================
 * BLOCK TEMPLATE
 * ============================================================================
 */

void block_template_init(block_template_t *tmpl) {
  if (tmpl == NULL)
    return;

  tmpl->version = 0x20000000; /* BIP9 version bits */
  memset(&tmpl->prev_hash, 0, sizeof(hash256_t));
  tmpl->curtime = 0;
  tmpl->bits = 0;
  tmpl->height = 0;
  tmpl->mintime = 0;
  tmpl->coinbase_value = 0;
  memset(&tmpl->target, 0, sizeof(hash256_t));
}

/*
 * ============================================================================
 * REGTEST GENESIS BLOCK
 * ============================================================================
 */

void block_genesis_header_testnet(block_header_t *header) {
  if (header == NULL)
    return;

  /* Clear header */
  memset(header, 0, sizeof(block_header_t));

  /* Same prev_hash as mainnet (all zeros) */
  /* header->prev_hash already zeroed */

  /* Same merkle root as mainnet (genesis coinbase txid) */
  memcpy(header->merkle_root.bytes, GENESIS_MERKLE_ROOT, 32);

  /* Testnet-specific values */
  header->version = TESTNET_GENESIS_VERSION;
  header->timestamp = TESTNET_GENESIS_TIMESTAMP;
  header->bits = TESTNET_GENESIS_BITS;
  header->nonce = TESTNET_GENESIS_NONCE;
}

void block_genesis_header_regtest(block_header_t *header) {
  if (header == NULL)
    return;

  /* Clear header */
  memset(header, 0, sizeof(block_header_t));

  /* Same prev_hash as mainnet (all zeros) */
  /* header->prev_hash already zeroed */

  /* Same merkle root as mainnet (genesis coinbase txid) */
  memcpy(header->merkle_root.bytes, GENESIS_MERKLE_ROOT, 32);

  /* Regtest-specific values */
  header->version = REGTEST_GENESIS_VERSION;
  header->timestamp = REGTEST_GENESIS_TIMESTAMP;
  header->bits = REGTEST_GENESIS_BITS;
  header->nonce = REGTEST_GENESIS_NONCE;
}

/*
 * ============================================================================
 * DIFFICULTY
 * ============================================================================
 */

uint32_t mining_get_difficulty_bits(uint32_t height, echo_bool_t is_regtest) {
  (void)height; /* Unused in regtest - always minimum difficulty */

  if (is_regtest) {
    /* Regtest: always use minimum difficulty */
    return REGTEST_POWLIMIT_BITS;
  }

  /* For non-regtest, this would consult the difficulty adjustment algorithm.
   * For now, return the mainnet genesis difficulty as a placeholder.
   * Full difficulty calculation is handled by the consensus layer. */
  return 0x1d00ffff;
}

/*
 * ============================================================================
 * NONCE MINING
 * ============================================================================
 */

echo_result_t mining_find_nonce(block_header_t *header, uint32_t max_nonce) {
  hash256_t hash;
  hash256_t target;

  if (header == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Convert bits to target */
  echo_result_t res = block_bits_to_target(header->bits, &target);
  if (res != ECHO_OK) {
    return res;
  }

  /* Try nonces from 0 to max_nonce */
  for (uint32_t nonce = 0; nonce <= max_nonce; nonce++) {
    header->nonce = nonce;

    res = block_header_hash(header, &hash);
    if (res != ECHO_OK) {
      return res;
    }

    if (block_hash_meets_target(&hash, &target)) {
      /* Found valid nonce */
      return ECHO_OK;
    }

    /* Check for wrap-around to avoid infinite loop */
    if (nonce == max_nonce) {
      break;
    }
  }

  return ECHO_ERR_NOT_FOUND;
}
