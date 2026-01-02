/*
 * Bitcoin Echo — Transaction Data Structures
 *
 * Implementation of transaction parsing, serialization, and ID computation.
 *
 * Build once. Build right. Stop.
 */

#include "tx.h"
#include "echo_types.h"
#include "serialize.h"
#include "sha256.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * SegWit marker and flag bytes.
 */
#define SEGWIT_MARKER 0x00
#define SEGWIT_FLAG 0x01

/*
 * Helper: free witness stack.
 */
static void witness_stack_free(witness_stack_t *ws) {
  size_t i;
  if (ws->items != NULL) {
    for (i = 0; i < ws->count; i++) {
      free(ws->items[i].data);
    }
    free(ws->items);
    ws->items = NULL;
  }
  ws->count = 0;
}

/*
 * Initialize a transaction structure.
 */
void tx_init(tx_t *tx) {
  if (tx == NULL)
    return;

  tx->version = 0;
  tx->inputs = NULL;
  tx->input_count = 0;
  tx->outputs = NULL;
  tx->output_count = 0;
  tx->locktime = 0;
  tx->has_witness = ECHO_FALSE;
}

/*
 * Free all memory owned by a transaction.
 */
void tx_free(tx_t *tx) {
  size_t i;

  if (tx == NULL)
    return;

  /* Free inputs */
  if (tx->inputs != NULL) {
    for (i = 0; i < tx->input_count; i++) {
      free(tx->inputs[i].script_sig);
      witness_stack_free(&tx->inputs[i].witness);
    }
    free(tx->inputs);
    tx->inputs = NULL;
  }
  tx->input_count = 0;

  /* Free outputs */
  if (tx->outputs != NULL) {
    for (i = 0; i < tx->output_count; i++) {
      free(tx->outputs[i].script_pubkey);
    }
    free(tx->outputs);
    tx->outputs = NULL;
  }
  tx->output_count = 0;

  tx->has_witness = ECHO_FALSE;
}

/*
 * Parse inputs from buffer.
 */
static echo_result_t parse_inputs(const uint8_t *data, size_t data_len,
                                  tx_t *tx, size_t *offset) {
  uint64_t count;
  size_t consumed;
  size_t i;
  echo_result_t result;

  /* Read input count */
  result = varint_read(data + *offset, data_len - *offset, &count, &consumed);
  if (result != ECHO_OK)
    return result;
  *offset += consumed;

  if (count > TX_MAX_INPUTS) {
    return ECHO_ERR_INVALID_FORMAT;
  }

  tx->input_count = (size_t)count;
  if (count == 0) {
    tx->inputs = NULL;
    return ECHO_OK;
  }

  tx->inputs = calloc(tx->input_count, sizeof(tx_input_t));
  if (tx->inputs == NULL) {
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  for (i = 0; i < tx->input_count; i++) {
    tx_input_t *input = &tx->inputs[i];
    uint64_t script_len;

    /* Initialize witness */
    input->witness.items = NULL;
    input->witness.count = 0;

    /* Previous output txid (32 bytes) */
    if (*offset + 32 > data_len) {
      return ECHO_ERR_TRUNCATED;
    }
    memcpy(input->prevout.txid.bytes, data + *offset, 32);
    *offset += 32;

    /* Previous output index (4 bytes) */
    if (*offset + 4 > data_len) {
      return ECHO_ERR_TRUNCATED;
    }
    input->prevout.vout = deserialize_u32_le(data + *offset);
    *offset += 4;

    /* ScriptSig length and data */
    result =
        varint_read(data + *offset, data_len - *offset, &script_len, &consumed);
    if (result != ECHO_OK)
      return result;
    *offset += consumed;

    if (script_len > TX_MAX_SCRIPT_SIZE) {
      return ECHO_ERR_INVALID_FORMAT;
    }

    input->script_sig_len = (size_t)script_len;
    if (script_len > 0) {
      if (*offset + script_len > data_len) {
        return ECHO_ERR_TRUNCATED;
      }
      input->script_sig = malloc(script_len);
      if (input->script_sig == NULL) {
        return ECHO_ERR_OUT_OF_MEMORY;
      }
      memcpy(input->script_sig, data + *offset, script_len);
      *offset += script_len;
    } else {
      input->script_sig = NULL;
    }

    /* Sequence (4 bytes) */
    if (*offset + 4 > data_len) {
      return ECHO_ERR_TRUNCATED;
    }
    input->sequence = deserialize_u32_le(data + *offset);
    *offset += 4;
  }

  return ECHO_OK;
}

/*
 * Parse outputs from buffer.
 */
static echo_result_t parse_outputs(const uint8_t *data, size_t data_len,
                                   tx_t *tx, size_t *offset) {
  uint64_t count;
  size_t consumed;
  size_t i;
  echo_result_t result;

  /* Read output count */
  result = varint_read(data + *offset, data_len - *offset, &count, &consumed);
  if (result != ECHO_OK)
    return result;
  *offset += consumed;

  if (count > TX_MAX_OUTPUTS) {
    return ECHO_ERR_INVALID_FORMAT;
  }

  tx->output_count = (size_t)count;
  if (count == 0) {
    tx->outputs = NULL;
    return ECHO_OK;
  }

  tx->outputs = calloc(tx->output_count, sizeof(tx_output_t));
  if (tx->outputs == NULL) {
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  for (i = 0; i < tx->output_count; i++) {
    tx_output_t *output = &tx->outputs[i];
    uint64_t script_len;

    /* Value (8 bytes) */
    if (*offset + 8 > data_len) {
      return ECHO_ERR_TRUNCATED;
    }
    output->value = (satoshi_t)deserialize_u64_le(data + *offset);
    *offset += 8;

    /* ScriptPubKey length and data */
    result =
        varint_read(data + *offset, data_len - *offset, &script_len, &consumed);
    if (result != ECHO_OK)
      return result;
    *offset += consumed;

    if (script_len > TX_MAX_SCRIPT_SIZE) {
      return ECHO_ERR_INVALID_FORMAT;
    }

    output->script_pubkey_len = (size_t)script_len;
    if (script_len > 0) {
      if (*offset + script_len > data_len) {
        return ECHO_ERR_TRUNCATED;
      }
      output->script_pubkey = malloc(script_len);
      if (output->script_pubkey == NULL) {
        return ECHO_ERR_OUT_OF_MEMORY;
      }
      memcpy(output->script_pubkey, data + *offset, script_len);
      *offset += script_len;
    } else {
      output->script_pubkey = NULL;
    }
  }

  return ECHO_OK;
}

/*
 * Parse witness data for all inputs.
 */
static echo_result_t parse_witness(const uint8_t *data, size_t data_len,
                                   tx_t *tx, size_t *offset) {
  size_t i, j;
  echo_result_t result;

  for (i = 0; i < tx->input_count; i++) {
    witness_stack_t *ws = &tx->inputs[i].witness;
    uint64_t item_count;
    size_t consumed;

    /* Read witness item count */
    result =
        varint_read(data + *offset, data_len - *offset, &item_count, &consumed);
    if (result != ECHO_OK)
      return result;
    *offset += consumed;

    ws->count = (size_t)item_count;
    if (item_count == 0) {
      ws->items = NULL;
      continue;
    }

    ws->items = calloc(ws->count, sizeof(witness_item_t));
    if (ws->items == NULL) {
      return ECHO_ERR_OUT_OF_MEMORY;
    }

    for (j = 0; j < ws->count; j++) {
      uint64_t item_len;

      result =
          varint_read(data + *offset, data_len - *offset, &item_len, &consumed);
      if (result != ECHO_OK)
        return result;
      *offset += consumed;

      ws->items[j].len = (size_t)item_len;
      if (item_len > 0) {
        if (*offset + item_len > data_len) {
          return ECHO_ERR_TRUNCATED;
        }
        ws->items[j].data = malloc(item_len);
        if (ws->items[j].data == NULL) {
          return ECHO_ERR_OUT_OF_MEMORY;
        }
        memcpy(ws->items[j].data, data + *offset, item_len);
        *offset += item_len;
      } else {
        ws->items[j].data = NULL;
      }
    }

    tx->has_witness = ECHO_TRUE;
  }

  return ECHO_OK;
}

/*
 * Parse a transaction from raw bytes.
 */
echo_result_t tx_parse(const uint8_t *data, size_t data_len, tx_t *tx,
                       size_t *consumed) {
  size_t offset = 0;
  echo_bool_t is_segwit = ECHO_FALSE;
  echo_result_t result;

  if (data == NULL || tx == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  tx_init(tx);

  /* Version (4 bytes) */
  if (data_len < 4) {
    return ECHO_ERR_TRUNCATED;
  }
  tx->version = (int32_t)deserialize_u32_le(data);
  offset = 4;

  /* Check for SegWit marker/flag */
  if (offset + 2 <= data_len && data[offset] == SEGWIT_MARKER) {
    if (data[offset + 1] == SEGWIT_FLAG) {
      is_segwit = ECHO_TRUE;
      offset += 2;
    } else if (data[offset + 1] == 0x00) {
      /* marker=0, flag=0 would be invalid, but input_count=0 is also invalid */
      return ECHO_ERR_INVALID_FORMAT;
    }
    /* If marker is 0 but flag is not 0 or 1, it's just input_count=0 which is
     * invalid */
  }

  /* Parse inputs */
  result = parse_inputs(data, data_len, tx, &offset);
  if (result != ECHO_OK) {
    tx_free(tx);
    return result;
  }

  /* For SegWit detection: if we thought it was legacy but input_count is 0,
   * error */
  if (!is_segwit && tx->input_count == 0) {
    tx_free(tx);
    return ECHO_ERR_INVALID_FORMAT;
  }

  /* Parse outputs */
  result = parse_outputs(data, data_len, tx, &offset);
  if (result != ECHO_OK) {
    tx_free(tx);
    return result;
  }

  /* Parse witness data if SegWit */
  if (is_segwit) {
    result = parse_witness(data, data_len, tx, &offset);
    if (result != ECHO_OK) {
      tx_free(tx);
      return result;
    }
  }

  /* Locktime (4 bytes) */
  if (offset + 4 > data_len) {
    tx_free(tx);
    return ECHO_ERR_TRUNCATED;
  }
  tx->locktime = deserialize_u32_le(data + offset);
  offset += 4;

  if (consumed != NULL) {
    *consumed = offset;
  }

  return ECHO_OK;
}

/*
 * Compute serialized size of a transaction.
 */
size_t tx_serialize_size(const tx_t *tx, echo_bool_t with_witness) {
  size_t size = 0;
  size_t i, j;
  echo_bool_t has_witness;

  if (tx == NULL)
    return 0;

  /* Check if we actually have witness data */
  has_witness = with_witness && tx->has_witness;

  /* Version (4 bytes) */
  size += 4;

  /* Marker and flag for SegWit (2 bytes) */
  if (has_witness) {
    size += 2;
  }

  /* Input count */
  size += varint_size(tx->input_count);

  /* Inputs */
  for (i = 0; i < tx->input_count; i++) {
    const tx_input_t *input = &tx->inputs[i];

    size += 32; /* prevout txid */
    size += 4;  /* prevout vout */
    size += varint_size(input->script_sig_len);
    size += input->script_sig_len;
    size += 4; /* sequence */
  }

  /* Output count */
  size += varint_size(tx->output_count);

  /* Outputs */
  for (i = 0; i < tx->output_count; i++) {
    const tx_output_t *output = &tx->outputs[i];

    size += 8; /* value */
    size += varint_size(output->script_pubkey_len);
    size += output->script_pubkey_len;
  }

  /* Witness data */
  if (has_witness) {
    for (i = 0; i < tx->input_count; i++) {
      const witness_stack_t *ws = &tx->inputs[i].witness;

      size += varint_size(ws->count);
      for (j = 0; j < ws->count; j++) {
        size += varint_size(ws->items[j].len);
        size += ws->items[j].len;
      }
    }
  }

  /* Locktime (4 bytes) */
  size += 4;

  return size;
}

/*
 * Serialize a transaction to bytes.
 */
echo_result_t tx_serialize(const tx_t *tx, echo_bool_t with_witness,
                           uint8_t *buf, size_t buf_len, size_t *written) {
  size_t offset = 0;
  size_t i, j;
  size_t needed;
  echo_bool_t has_witness;
  echo_result_t result;
  size_t var_written;

  if (tx == NULL || buf == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Check buffer size */
  needed = tx_serialize_size(tx, with_witness);
  if (buf_len < needed) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }

  /* Check if we actually have witness data */
  has_witness = with_witness && tx->has_witness;

  /* Version */
  serialize_u32_le(buf + offset, (uint32_t)tx->version);
  offset += 4;

  /* Marker and flag for SegWit */
  if (has_witness) {
    buf[offset++] = SEGWIT_MARKER;
    buf[offset++] = SEGWIT_FLAG;
  }

  /* Input count */
  result = varint_write(buf + offset, buf_len - offset, tx->input_count,
                        &var_written);
  if (result != ECHO_OK)
    return result;
  offset += var_written;

  /* Inputs */
  for (i = 0; i < tx->input_count; i++) {
    const tx_input_t *input = &tx->inputs[i];

    /* Prevout txid */
    memcpy(buf + offset, input->prevout.txid.bytes, 32);
    offset += 32;

    /* Prevout vout */
    serialize_u32_le(buf + offset, input->prevout.vout);
    offset += 4;

    /* ScriptSig */
    result = varint_write(buf + offset, buf_len - offset, input->script_sig_len,
                          &var_written);
    if (result != ECHO_OK)
      return result;
    offset += var_written;

    if (input->script_sig_len > 0) {
      memcpy(buf + offset, input->script_sig, input->script_sig_len);
      offset += input->script_sig_len;
    }

    /* Sequence */
    serialize_u32_le(buf + offset, input->sequence);
    offset += 4;
  }

  /* Output count */
  result = varint_write(buf + offset, buf_len - offset, tx->output_count,
                        &var_written);
  if (result != ECHO_OK)
    return result;
  offset += var_written;

  /* Outputs */
  for (i = 0; i < tx->output_count; i++) {
    const tx_output_t *output = &tx->outputs[i];

    /* Value */
    serialize_u64_le(buf + offset, (uint64_t)output->value);
    offset += 8;

    /* ScriptPubKey */
    result = varint_write(buf + offset, buf_len - offset,
                          output->script_pubkey_len, &var_written);
    if (result != ECHO_OK)
      return result;
    offset += var_written;

    if (output->script_pubkey_len > 0) {
      memcpy(buf + offset, output->script_pubkey, output->script_pubkey_len);
      offset += output->script_pubkey_len;
    }
  }

  /* Witness data */
  if (has_witness) {
    for (i = 0; i < tx->input_count; i++) {
      const witness_stack_t *ws = &tx->inputs[i].witness;

      result =
          varint_write(buf + offset, buf_len - offset, ws->count, &var_written);
      if (result != ECHO_OK)
        return result;
      offset += var_written;

      for (j = 0; j < ws->count; j++) {
        result = varint_write(buf + offset, buf_len - offset, ws->items[j].len,
                              &var_written);
        if (result != ECHO_OK)
          return result;
        offset += var_written;

        if (ws->items[j].len > 0) {
          memcpy(buf + offset, ws->items[j].data, ws->items[j].len);
          offset += ws->items[j].len;
        }
      }
    }
  }

  /* Locktime */
  serialize_u32_le(buf + offset, tx->locktime);
  offset += 4;

  if (written != NULL) {
    *written = offset;
  }

  return ECHO_OK;
}

/*
 * Compute the transaction ID (txid).
 */
echo_result_t tx_compute_txid(const tx_t *tx, hash256_t *txid) {
  uint8_t *buf;
  size_t size;
  echo_result_t result;

  if (tx == NULL || txid == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Serialize without witness */
  size = tx_serialize_size(tx, ECHO_FALSE);
  buf = malloc(size);
  if (buf == NULL) {
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  result = tx_serialize(tx, ECHO_FALSE, buf, size, NULL);
  if (result != ECHO_OK) {
    free(buf);
    return result;
  }

  /* SHA256d */
  sha256d(buf, size, txid->bytes);

  free(buf);
  return ECHO_OK;
}

/*
 * Compute the witness transaction ID (wtxid).
 */
echo_result_t tx_compute_wtxid(const tx_t *tx, hash256_t *wtxid) {
  uint8_t *buf;
  size_t size;
  echo_result_t result;

  if (tx == NULL || wtxid == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Serialize with witness */
  size = tx_serialize_size(tx, ECHO_TRUE);
  buf = malloc(size);
  if (buf == NULL) {
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  result = tx_serialize(tx, ECHO_TRUE, buf, size, NULL);
  if (result != ECHO_OK) {
    free(buf);
    return result;
  }

  /* SHA256d */
  sha256d(buf, size, wtxid->bytes);

  free(buf);
  return ECHO_OK;
}

/*
 * Check if a transaction is a coinbase transaction.
 */
echo_bool_t tx_is_coinbase(const tx_t *tx) {
  size_t i;
  echo_bool_t all_zero;

  if (tx == NULL)
    return ECHO_FALSE;
  if (tx->input_count != 1)
    return ECHO_FALSE;

  /* Check for null prevout: all-zero txid and vout = 0xFFFFFFFF */
  all_zero = ECHO_TRUE;
  for (i = 0; i < 32; i++) {
    if (tx->inputs[0].prevout.txid.bytes[i] != 0) {
      all_zero = ECHO_FALSE;
      break;
    }
  }

  return all_zero && (tx->inputs[0].prevout.vout == TX_COINBASE_VOUT);
}

/*
 * Compute transaction weight.
 */
size_t tx_weight(const tx_t *tx) {
  size_t base_size;
  size_t total_size;

  if (tx == NULL)
    return 0;

  base_size = tx_serialize_size(tx, ECHO_FALSE);
  total_size = tx_serialize_size(tx, ECHO_TRUE);

  /* Weight = (base_size * 3) + total_size = (base_size * 4) + witness_size */
  return (base_size * 3) + total_size;
}

/*
 * Compute transaction virtual size.
 */
size_t tx_vsize(const tx_t *tx) {
  size_t weight;

  if (tx == NULL)
    return 0;

  weight = tx_weight(tx);

  /* vsize = ceil(weight / 4) */
  return (weight + 3) / 4;
}
