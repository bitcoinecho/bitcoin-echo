/**
 * Bitcoin Echo — Protocol Message Utilities
 *
 * Implementation of protocol message parsing and utility functions.
 */

#include "echo_types.h"
#include "protocol.h"
#include "sha256.h"
#include <stdint.h>
#include <string.h>

/* Command name mapping table */
static const struct {
  msg_type_t type;
  const char *command;
} command_table[] = {
    {MSG_VERSION, "version"},
    {MSG_VERACK, "verack"},
    {MSG_PING, "ping"},
    {MSG_PONG, "pong"},
    {MSG_INV, "inv"},
    {MSG_GETDATA, "getdata"},
    {MSG_NOTFOUND, "notfound"},
    {MSG_BLOCK, "block"},
    {MSG_TX, "tx"},
    {MSG_ADDR, "addr"},
    {MSG_GETADDR, "getaddr"},
    {MSG_GETHEADERS, "getheaders"},
    {MSG_GETBLOCKS, "getblocks"},
    {MSG_HEADERS, "headers"},
    {MSG_REJECT, "reject"},
    {MSG_SENDHEADERS, "sendheaders"},
    {MSG_FEEFILTER, "feefilter"},
    {MSG_SENDCMPCT, "sendcmpct"},
    {MSG_WTXIDRELAY, "wtxidrelay"},
};

#define COMMAND_TABLE_SIZE (sizeof(command_table) / sizeof(command_table[0]))

/**
 * Parse message type from command string.
 */
msg_type_t msg_parse_command(const char *command) {
  /* Command string must be null-terminated within COMMAND_LEN */
  size_t cmd_len = 0;
  for (size_t i = 0; i < COMMAND_LEN; i++) {
    if (command[i] == '\0') {
      cmd_len = i;
      break;
    }
  }

  /* If no null terminator found, invalid command */
  if (cmd_len == 0 || cmd_len == COMMAND_LEN) {
    return MSG_UNKNOWN;
  }

  /* Look up in command table */
  for (size_t i = 0; i < COMMAND_TABLE_SIZE; i++) {
    if (strcmp(command, command_table[i].command) == 0) {
      return command_table[i].type;
    }
  }

  return MSG_UNKNOWN;
}

/**
 * Get command string for message type.
 */
const char *msg_command_string(msg_type_t type) {
  for (size_t i = 0; i < COMMAND_TABLE_SIZE; i++) {
    if (command_table[i].type == type) {
      return command_table[i].command;
    }
  }
  return NULL;
}

/**
 * Compute message checksum.
 *
 * Checksum is first 4 bytes of SHA256d(payload).
 */
uint32_t msg_checksum(const uint8_t *payload, size_t len) {
  hash256_t hash;
  sha256d(payload, len, hash.bytes);

  /* Extract first 4 bytes as little-endian uint32 */
  uint32_t checksum = 0;
  checksum |= ((uint32_t)hash.bytes[0]) << 0;
  checksum |= ((uint32_t)hash.bytes[1]) << 8;
  checksum |= ((uint32_t)hash.bytes[2]) << 16;
  checksum |= ((uint32_t)hash.bytes[3]) << 24;

  return checksum;
}

/**
 * Validate message header.
 */
echo_bool_t msg_header_valid(const msg_header_t *header,
                             uint32_t expected_magic) {
  /* Check magic bytes */
  if (header->magic != expected_magic) {
    return ECHO_FALSE;
  }

  /* Verify command is null-terminated */
  echo_bool_t has_null = ECHO_FALSE;
  for (size_t i = 0; i < COMMAND_LEN; i++) {
    if (header->command[i] == '\0') {
      has_null = ECHO_TRUE;
      break;
    }
  }
  if (!has_null) {
    return ECHO_FALSE;
  }

  /* Check length is within bounds */
  if (header->length > MAX_MESSAGE_SIZE) {
    return ECHO_FALSE;
  }

  return ECHO_TRUE;
}
