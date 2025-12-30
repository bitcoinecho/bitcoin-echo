/**
 * Bitcoin Echo — Peer Connection Management
 *
 * Implements the lifecycle of P2P connections including:
 * - Connection establishment and acceptance
 * - Version/verack handshake
 * - Message queueing and transmission
 * - Disconnection handling
 *
 * Design principles:
 * - Each peer is single-threaded (no internal locking)
 * - Caller is responsible for synchronization if needed
 * - Blocking I/O operations (platform layer handles this)
 * - Simple state machine for connection lifecycle
 *
 * Build once. Build right. Stop.
 */

#include "peer.h"
#include "echo_types.h"
#include "log.h"
#include "platform.h"
#include "protocol.h"
#include "protocol_serialize.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* User agent string identifying this node */
#define USER_AGENT "/BitcoinEcho:0.1.0/"

/* Minimum protocol version we support */
#define MIN_PROTOCOL_VERSION 70001

/**
 * Initialize peer structure to default state.
 */
void peer_init(peer_t *peer) {
  memset(peer, 0, sizeof(peer_t));
  peer->state = PEER_STATE_DISCONNECTED;
  peer->disconnect_reason = PEER_DISCONNECT_NONE;
  peer->socket = NULL;
}

/**
 * Start async outbound connection to remote peer.
 *
 * Returns ECHO_SUCCESS if connection started (peer in CONNECTING state).
 * Returns ECHO_OK if connected immediately (peer in CONNECTED state).
 * Caller must call peer_check_connect() to poll for completion.
 */
echo_result_t peer_connect(peer_t *peer, const char *address, uint16_t port,
                           uint64_t nonce) {
  if (!peer || !address) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Initialize peer structure */
  peer_init(peer);
  peer->inbound = ECHO_FALSE;
  peer->nonce_local = nonce;
  peer->port = port;

  /* Copy address string */
  size_t addr_len = strlen(address);
  if (addr_len >= sizeof(peer->address)) {
    return ECHO_ERR_INVALID_PARAM;
  }
  memcpy(peer->address, address, addr_len + 1);

  /* Allocate socket */
  peer->socket = plat_socket_alloc();
  if (!peer->socket) {
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  /* Create socket */
  if (plat_socket_create(peer->socket) != PLAT_OK) {
    plat_socket_free(peer->socket);
    peer->socket = NULL;
    return ECHO_ERR_NETWORK;
  }

  /* Start async connect - returns immediately */
  int result = plat_socket_connect_async(peer->socket, address, port);

  if (result == PLAT_PENDING) {
    /* Connection in progress - stay in CONNECTING state */
    peer->state = PEER_STATE_CONNECTING;
    peer->connect_time = plat_time_ms(); /* Track when connect started */
    return ECHO_SUCCESS; /* Caller must poll with peer_check_connect() */
  } else if (result == PLAT_OK) {
    /* Immediate connection (rare, localhost) */
    peer->state = PEER_STATE_CONNECTED;
    peer->connect_time = plat_time_ms();
    peer->last_recv = peer->connect_time;
    peer->last_send = peer->connect_time;
    return ECHO_OK;
  } else {
    /* Connection failed immediately */
    peer->state = PEER_STATE_DISCONNECTED;
    peer->disconnect_reason = PEER_DISCONNECT_NETWORK_ERROR;
    snprintf(peer->disconnect_message, sizeof(peer->disconnect_message),
             "Failed to connect to %s:%u", address, port);
    plat_socket_close(peer->socket);
    plat_socket_free(peer->socket);
    peer->socket = NULL;
    return ECHO_ERR_NETWORK;
  }
}

/**
 * Check if a pending async connection has completed.
 *
 * Returns:
 *   ECHO_SUCCESS - Connection in progress, check again later
 *   ECHO_OK      - Connection complete, peer is now CONNECTED
 *   ECHO_ERR_*   - Connection failed, peer is DISCONNECTED
 */
echo_result_t peer_check_connect(peer_t *peer) {
  if (!peer || peer->state != PEER_STATE_CONNECTING) {
    return ECHO_ERR_INVALID_PARAM;
  }

  int result = plat_socket_check_connect(peer->socket);

  if (result == PLAT_PENDING) {
    /* Still connecting - check timeout (2 seconds) */
    uint64_t elapsed = plat_time_ms() - peer->connect_time;
    if (elapsed > 2000) {
      /* Connection timeout */
      peer->state = PEER_STATE_DISCONNECTED;
      peer->disconnect_reason = PEER_DISCONNECT_TIMEOUT;
      snprintf(peer->disconnect_message, sizeof(peer->disconnect_message),
               "Connection timeout to %s:%u", peer->address, peer->port);
      plat_socket_close(peer->socket);
      plat_socket_free(peer->socket);
      peer->socket = NULL;
      return ECHO_ERR_TIMEOUT;
    }
    return ECHO_SUCCESS; /* Still pending */
  } else if (result == PLAT_OK) {
    /* Connection succeeded! */
    peer->state = PEER_STATE_CONNECTED;
    uint64_t now = plat_time_ms();
    peer->connect_time = now; /* Reset connect_time to NOW, not async start */
    peer->last_recv = now;
    peer->last_send = now;
    return ECHO_OK;
  } else {
    /* Connection failed */
    peer->state = PEER_STATE_DISCONNECTED;
    peer->disconnect_reason = PEER_DISCONNECT_NETWORK_ERROR;
    snprintf(peer->disconnect_message, sizeof(peer->disconnect_message),
             "Connection failed to %s:%u", peer->address, peer->port);
    plat_socket_close(peer->socket);
    plat_socket_free(peer->socket);
    peer->socket = NULL;
    return ECHO_ERR_NETWORK;
  }
}

/**
 * Accept inbound connection from listening socket.
 */
echo_result_t peer_accept(peer_t *peer, plat_socket_t *listener,
                          uint64_t nonce) {
  if (!peer || !listener) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Initialize peer structure */
  peer_init(peer);
  peer->inbound = ECHO_TRUE;
  peer->nonce_local = nonce;

  /* Allocate socket */
  peer->socket = plat_socket_alloc();
  if (!peer->socket) {
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  /* Accept connection */
  if (plat_socket_accept(listener, peer->socket) != PLAT_OK) {
    plat_socket_free(peer->socket);
    peer->socket = NULL;
    return ECHO_ERR_NETWORK;
  }

  /* Set socket to non-blocking for event loop compatibility */
  plat_socket_set_nonblocking(peer->socket);

  /* Connection established */
  peer->state = PEER_STATE_CONNECTED;
  peer->connect_time = plat_time_ms();
  peer->last_recv = peer->connect_time;
  peer->last_send = peer->connect_time;

  /* Note: We don't know the peer's address from accept() alone.
   * In a full implementation, we'd use getpeername() here.
   * For now, leave address empty for inbound connections. */
  snprintf(peer->address, sizeof(peer->address), "(inbound)");

  return ECHO_SUCCESS;
}

/**
 * Send version message to initiate handshake.
 */
echo_result_t peer_send_version(peer_t *peer, uint64_t our_services,
                                int32_t our_height, echo_bool_t relay) {
  if (!peer) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Must be in CONNECTED state */
  if (peer->state != PEER_STATE_CONNECTED) {
    return ECHO_ERR_INVALID_STATE;
  }

  /* Build version message */
  msg_version_t version_msg;
  memset(&version_msg, 0, sizeof(version_msg));

  version_msg.version = PROTOCOL_VERSION;
  version_msg.services = our_services;
  version_msg.timestamp =
      (int64_t)plat_time_ms() / 1000; /* Convert to seconds */

  /* Receiving node address (peer) - we don't know their services yet */
  memset(&version_msg.addr_recv, 0, sizeof(version_msg.addr_recv));
  version_msg.addr_recv.services = 0;
  version_msg.addr_recv.port = peer->port;

  /* Sending node address (us) */
  memset(&version_msg.addr_from, 0, sizeof(version_msg.addr_from));
  version_msg.addr_from.services = our_services;

  version_msg.nonce = peer->nonce_local;

  /* User agent */
  size_t ua_len = strlen(USER_AGENT);
  memcpy(version_msg.user_agent, USER_AGENT, ua_len);
  version_msg.user_agent_len = ua_len;

  version_msg.start_height = our_height;
  version_msg.relay = relay;

  /* Serialize version message payload */
  uint8_t payload_buf[1024];
  size_t payload_len;
  echo_result_t result = msg_version_serialize(
      &version_msg, payload_buf, sizeof(payload_buf), &payload_len);
  if (result != ECHO_SUCCESS) {
    return result;
  }

  /* Build message header */
  msg_header_t header;
  header.magic = MAGIC_MAINNET;
  memset(header.command, 0, COMMAND_LEN);
  memcpy(header.command, "version", 7);
  header.length = (uint32_t)payload_len;
  header.checksum = msg_checksum(payload_buf, payload_len);

  /* Serialize header */
  uint8_t header_buf[24];
  result = msg_header_serialize(&header, header_buf, sizeof(header_buf));
  if (result != ECHO_SUCCESS) {
    return result;
  }

  /* Send header */
  int sent = plat_socket_send(peer->socket, header_buf, 24);
  if (sent != 24) {
    peer_disconnect(peer, PEER_DISCONNECT_NETWORK_ERROR,
                    "Failed to send version header");
    return ECHO_ERR_NETWORK;
  }

  /* Send payload */
  sent = plat_socket_send(peer->socket, payload_buf, payload_len);
  if (sent != (int)payload_len) {
    peer_disconnect(peer, PEER_DISCONNECT_NETWORK_ERROR,
                    "Failed to send version payload");
    return ECHO_ERR_NETWORK;
  }

  peer->last_send = plat_time_ms();
  peer->bytes_sent += 24 + payload_len;
  peer->messages_sent++;

  /* Transition to handshake state */
  peer->state = PEER_STATE_HANDSHAKE_SENT;

  return ECHO_SUCCESS;
}

/**
 * Process incoming data and parse messages.
 */
echo_result_t peer_receive(peer_t *peer, msg_t *msg) {
  if (!peer || !msg) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Must be connected */
  if (peer->state == PEER_STATE_DISCONNECTED ||
      peer->state == PEER_STATE_DISCONNECTING) {
    return ECHO_ERR_INVALID_STATE;
  }

  /* Try to receive more data */
  size_t available = (size_t)PEER_RECV_BUFFER_SIZE - peer->recv_buffer_len;
  if (available > 0) {
    int received = plat_socket_recv(
        peer->socket, peer->recv_buffer + peer->recv_buffer_len, available);
    if (received == PLAT_ERR_WOULD_BLOCK) {
      /* No data available right now - not an error for non-blocking socket */
      /* Fall through to check if we have a complete message buffered */
    } else if (received < 0) {
      /* Network error */
      peer_disconnect(peer, PEER_DISCONNECT_NETWORK_ERROR,
                      "Socket receive error");
      return ECHO_ERR_NETWORK;
    } else if (received == 0) {
      /* Connection closed by peer */
      peer_disconnect(peer, PEER_DISCONNECT_PEER_CLOSED,
                      "Connection closed by peer");
      return ECHO_ERR_NETWORK;
    } else {
      peer->recv_buffer_len += (size_t)received;
      peer->bytes_recv += (uint64_t)received;
      peer->last_recv = plat_time_ms();
    }
  }

  /* Do we have a complete message header? */
  if (peer->recv_buffer_len < 24) {
    return ECHO_ERR_WOULD_BLOCK; /* Need more data */
  }

  /* Parse message header */
  msg_header_t header;
  echo_result_t result = msg_header_deserialize(peer->recv_buffer, 24, &header);
  if (result != ECHO_SUCCESS) {
    peer_disconnect(peer, PEER_DISCONNECT_PROTOCOL_ERROR,
                    "Invalid message header");
    return ECHO_ERR_PROTOCOL;
  }

  /* Validate magic bytes */
  if (header.magic != MAGIC_MAINNET) {
    peer_disconnect(peer, PEER_DISCONNECT_PROTOCOL_ERROR,
                    "Invalid network magic");
    return ECHO_ERR_PROTOCOL;
  }

  /* Validate payload length */
  if (header.length > MAX_MESSAGE_SIZE) {
    peer_disconnect(peer, PEER_DISCONNECT_PROTOCOL_ERROR, "Message too large");
    return ECHO_ERR_PROTOCOL;
  }

  /* Do we have the complete payload? */
  size_t total_size = 24 + header.length;
  if (peer->recv_buffer_len < total_size) {
    return ECHO_ERR_WOULD_BLOCK; /* Need more data */
  }

  /* Validate checksum */
  uint8_t *payload = peer->recv_buffer + 24;
  uint32_t computed_checksum = msg_checksum(payload, header.length);
  if (computed_checksum != header.checksum) {
    peer_disconnect(peer, PEER_DISCONNECT_PROTOCOL_ERROR,
                    "Invalid message checksum");
    return ECHO_ERR_PROTOCOL;
  }

  /* Parse message type */
  msg_type_t type = msg_parse_command(header.command);

  /* Deserialize payload based on message type */
  memset(msg, 0, sizeof(msg_t));
  msg->type = type;

  size_t consumed = 0;

  switch (type) {
  case MSG_VERSION:
    result = msg_version_deserialize(payload, header.length,
                                     &msg->payload.version, &consumed);
    if (result == ECHO_SUCCESS) {
      /* Handle version message */
      peer->version = msg->payload.version.version;
      peer->services = msg->payload.version.services;
      peer->start_height = msg->payload.version.start_height;
      peer->nonce_remote = msg->payload.version.nonce;
      peer->relay = msg->payload.version.relay;

      /* Copy user agent */
      size_t ua_len = msg->payload.version.user_agent_len;
      if (ua_len > 0 && ua_len < MAX_USER_AGENT_LEN) {
        memcpy(peer->user_agent, msg->payload.version.user_agent, ua_len);
        peer->user_agent_len = ua_len;
      }

      /* Check for self-connection */
      if (peer->nonce_remote == peer->nonce_local) {
        peer_disconnect(peer, PEER_DISCONNECT_PROTOCOL_ERROR,
                        "Self-connection detected");
        return ECHO_ERR_PROTOCOL;
      }

      /* Check protocol version */
      if (peer->version < MIN_PROTOCOL_VERSION) {
        peer_disconnect(peer, PEER_DISCONNECT_PROTOCOL_ERROR,
                        "Protocol version too old");
        return ECHO_ERR_PROTOCOL;
      }

      /* Update state based on current state */
      if (peer->state == PEER_STATE_CONNECTED ||
          peer->state == PEER_STATE_HANDSHAKE_SENT) {
        /* Received version - move to handshake received state */
        peer->state = PEER_STATE_HANDSHAKE_RECV;
      }
    }
    break;

  case MSG_VERACK:
    /* verack has no payload */
    if (peer->state == PEER_STATE_HANDSHAKE_RECV) {
      /* Handshake complete */
      peer->state = PEER_STATE_READY;
    }
    break;

  case MSG_PING:
    result = msg_ping_deserialize(payload, header.length, &msg->payload.ping,
                                  &consumed);
    break;

  case MSG_PONG:
    result = msg_pong_deserialize(payload, header.length, &msg->payload.pong,
                                  &consumed);
    break;

  case MSG_INV:
    result = msg_inv_deserialize(payload, header.length, &msg->payload.inv,
                                 &consumed);
    break;

  case MSG_GETDATA:
    result = msg_inv_deserialize(payload, header.length, &msg->payload.getdata,
                                 &consumed);
    break;

  case MSG_ADDR:
    result = msg_addr_deserialize(payload, header.length, &msg->payload.addr,
                                  &consumed);
    break;

  case MSG_GETHEADERS:
    result = msg_getheaders_deserialize(payload, header.length,
                                        &msg->payload.getheaders, &consumed);
    break;

  case MSG_HEADERS:
    result = msg_headers_deserialize(payload, header.length,
                                     &msg->payload.headers, &consumed);
    break;

  case MSG_BLOCK:
    log_info(LOG_COMP_NET, "RECV block message: %u bytes from %s:%u",
             header.length, peer->address, peer->port);
    result = msg_block_deserialize(payload, header.length, &msg->payload.block,
                                   &consumed);
    if (result != ECHO_SUCCESS) {
      log_error(LOG_COMP_NET, "Block deserialize failed: %d", result);
    }
    break;

  case MSG_TX:
    result =
        msg_tx_deserialize(payload, header.length, &msg->payload.tx, &consumed);
    break;

  case MSG_REJECT:
    result = msg_reject_deserialize(payload, header.length,
                                    &msg->payload.reject, &consumed);
    break;

  case MSG_FEEFILTER:
    result = msg_feefilter_deserialize(payload, header.length,
                                       &msg->payload.feefilter, &consumed);
    break;

  case MSG_SENDCMPCT:
    result = msg_sendcmpct_deserialize(payload, header.length,
                                       &msg->payload.sendcmpct, &consumed);
    break;

  case MSG_GETADDR:
  case MSG_SENDHEADERS:
  case MSG_WTXIDRELAY:
  case MSG_UNKNOWN:
  default:
    /* These messages have no payload, or unknown message - ignore */
    result = ECHO_SUCCESS;
    break;
  }

  if (result != ECHO_SUCCESS) {
    log_info(LOG_COMP_NET, "Failed to parse %s payload (type=%d, len=%u, err=%d)",
             msg_command_string(type), type, header.length, result);
    peer_disconnect(peer, PEER_DISCONNECT_PROTOCOL_ERROR,
                    "Failed to parse message payload");
    return ECHO_ERR_PROTOCOL;
  }

  peer->messages_recv++;

  /* Remove message from receive buffer */
  peer->recv_buffer_len -= total_size;
  if (peer->recv_buffer_len > 0) {
    memmove(peer->recv_buffer, peer->recv_buffer + total_size,
            peer->recv_buffer_len);
  }

  return ECHO_SUCCESS;
}

/**
 * Queue message for sending.
 *
 * Messages with pointer fields (inv, getdata, notfound) are deep-copied
 * to avoid use-after-free when the caller's stack data goes out of scope.
 */
echo_result_t peer_queue_message(peer_t *peer, const msg_t *msg) {
  if (!peer || !msg) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Check if peer is ready */
  if (peer->state != PEER_STATE_READY &&
      peer->state != PEER_STATE_HANDSHAKE_RECV) {
    /* Special case: allow verack during handshake */
    if (!(peer->state == PEER_STATE_HANDSHAKE_RECV &&
          msg->type == MSG_VERACK)) {
      return ECHO_ERR_INVALID_STATE;
    }
  }

  /* Check if queue is full */
  if (peer->send_queue_count >= PEER_SEND_QUEUE_SIZE) {
    return ECHO_ERR_FULL;
  }

  /* Add message to queue */
  peer_msg_queue_entry_t *entry = &peer->send_queue[peer->send_queue_tail];
  entry->message = *msg;
  entry->allocated = ECHO_FALSE;

  /*
   * Deep copy pointer fields for messages that have them.
   * This prevents use-after-free when callers pass stack-allocated data.
   */
  switch (msg->type) {
  case MSG_INV:
  case MSG_GETDATA:
  case MSG_NOTFOUND:
    if (msg->payload.inv.count > 0 && msg->payload.inv.inventory != NULL) {
      size_t size = msg->payload.inv.count * sizeof(inv_vector_t);
      inv_vector_t *copy = malloc(size);
      if (!copy) {
        return ECHO_ERR_MEMORY;
      }
      memcpy(copy, msg->payload.inv.inventory, size);
      entry->message.payload.inv.inventory = copy;
      entry->allocated = ECHO_TRUE;
    }
    break;
  default:
    /* Other message types don't have pointer fields we need to copy */
    break;
  }

  peer->send_queue_tail = (peer->send_queue_tail + 1) % PEER_SEND_QUEUE_SIZE;
  peer->send_queue_count++;

  return ECHO_SUCCESS;
}

/**
 * Helper function to serialize and send a single message.
 */
static echo_result_t peer_send_message_internal(peer_t *peer,
                                                const msg_t *msg) {
  uint8_t header_buf[24];
  uint8_t payload_buf[20480]; /* 20KB: supports getdata with up to 555 blocks */
  size_t payload_len = 0;
  echo_result_t result;

  /* Serialize payload based on message type */
  switch (msg->type) {
  case MSG_VERSION:
    result = msg_version_serialize(&msg->payload.version, payload_buf,
                                   sizeof(payload_buf), &payload_len);
    break;

  case MSG_VERACK:
  case MSG_GETADDR:
  case MSG_SENDHEADERS:
  case MSG_WTXIDRELAY:
    /* No payload */
    payload_len = 0;
    result = ECHO_SUCCESS;
    break;

  case MSG_PING:
    result = msg_ping_serialize(&msg->payload.ping, payload_buf,
                                sizeof(payload_buf), &payload_len);
    break;

  case MSG_PONG:
    result = msg_pong_serialize(&msg->payload.pong, payload_buf,
                                sizeof(payload_buf), &payload_len);
    break;

  case MSG_INV:
    result = msg_inv_serialize(&msg->payload.inv, payload_buf,
                               sizeof(payload_buf), &payload_len);
    break;

  case MSG_GETDATA:
    result = msg_inv_serialize(&msg->payload.getdata, payload_buf,
                               sizeof(payload_buf), &payload_len);
    break;

  case MSG_ADDR:
    result = msg_addr_serialize(&msg->payload.addr, payload_buf,
                                sizeof(payload_buf), &payload_len);
    break;

  case MSG_GETHEADERS:
    result = msg_getheaders_serialize(&msg->payload.getheaders, payload_buf,
                                      sizeof(payload_buf), &payload_len);
    break;

  case MSG_HEADERS:
    result = msg_headers_serialize(&msg->payload.headers, payload_buf,
                                   sizeof(payload_buf), &payload_len);
    break;

  case MSG_FEEFILTER:
    result = msg_feefilter_serialize(&msg->payload.feefilter, payload_buf,
                                     sizeof(payload_buf), &payload_len);
    break;

  case MSG_SENDCMPCT:
    result = msg_sendcmpct_serialize(&msg->payload.sendcmpct, payload_buf,
                                     sizeof(payload_buf), &payload_len);
    break;

  default:
    return ECHO_ERR_INVALID_PARAM;
  }

  if (result != ECHO_SUCCESS) {
    return result;
  }

  /* Build message header */
  msg_header_t header;
  header.magic = MAGIC_MAINNET;
  memset(header.command, 0, COMMAND_LEN);
  const char *cmd = msg_command_string(msg->type);
  if (cmd) {
    size_t cmd_len = strlen(cmd);
    memcpy(header.command, cmd, cmd_len);
  }
  header.length = (uint32_t)payload_len;
  header.checksum = msg_checksum(payload_buf, payload_len);

  /* Serialize header */
  result = msg_header_serialize(&header, header_buf, sizeof(header_buf));
  if (result != ECHO_SUCCESS) {
    return result;
  }

  /* Send header */
  int sent = plat_socket_send(peer->socket, header_buf, 24);
  if (sent != 24) {
    return ECHO_ERR_NETWORK;
  }

  /* Send payload if present */
  if (payload_len > 0) {
    sent = plat_socket_send(peer->socket, payload_buf, payload_len);
    if (sent != (int)payload_len) {
      return ECHO_ERR_NETWORK;
    }
  }

  peer->last_send = plat_time_ms();
  peer->bytes_sent += 24 + payload_len;
  peer->messages_sent++;

  /* Debug: Log when getdata is actually sent */
  if (msg->type == MSG_GETDATA) {
    log_info(LOG_COMP_NET, "SENT getdata: %zu items, %zu bytes to %s:%u",
             msg->payload.getdata.count, 24 + payload_len, peer->address,
             peer->port);
  }

  return ECHO_SUCCESS;
}

/**
 * Free dynamically allocated message data.
 */
static void peer_free_message_data(peer_msg_queue_entry_t *entry) {
  if (!entry->allocated) {
    return;
  }

  switch (entry->message.type) {
  case MSG_INV:
  case MSG_GETDATA:
  case MSG_NOTFOUND:
    free(entry->message.payload.inv.inventory);
    entry->message.payload.inv.inventory = NULL;
    break;
  default:
    break;
  }

  entry->allocated = ECHO_FALSE;
}

/**
 * Send queued messages.
 */
echo_result_t peer_send_queued(peer_t *peer) {
  if (!peer) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Send all queued messages */
  while (peer->send_queue_count > 0) {
    peer_msg_queue_entry_t *entry = &peer->send_queue[peer->send_queue_head];

    echo_result_t result = peer_send_message_internal(peer, &entry->message);

    /* Free allocated data regardless of send result */
    peer_free_message_data(entry);

    if (result != ECHO_SUCCESS) {
      peer_disconnect(peer, PEER_DISCONNECT_NETWORK_ERROR,
                      "Failed to send message");
      return ECHO_ERR_NETWORK;
    }

    /* Remove from queue */
    peer->send_queue_head = (peer->send_queue_head + 1) % PEER_SEND_QUEUE_SIZE;
    peer->send_queue_count--;
  }

  return ECHO_SUCCESS;
}

/**
 * Disconnect peer.
 */
void peer_disconnect(peer_t *peer, peer_disconnect_reason_t reason,
                     const char *message) {
  if (!peer) {
    return;
  }

  if (peer->state == PEER_STATE_DISCONNECTED) {
    return; /* Already disconnected */
  }

  log_info(LOG_COMP_NET, "Peer %s disconnecting: reason=%s, message=%s",
           peer->address, peer_disconnect_reason_string(reason),
           message ? message : "(none)");

  peer->state = PEER_STATE_DISCONNECTED;
  peer->disconnect_reason = reason;

  if (message) {
    size_t msg_len = strlen(message);
    if (msg_len >= sizeof(peer->disconnect_message)) {
      msg_len = sizeof(peer->disconnect_message) - 1;
    }
    memcpy(peer->disconnect_message, message, msg_len);
    peer->disconnect_message[msg_len] = '\0';
  }

  /* Free any remaining allocated messages in the send queue */
  while (peer->send_queue_count > 0) {
    peer_msg_queue_entry_t *entry = &peer->send_queue[peer->send_queue_head];
    peer_free_message_data(entry);
    peer->send_queue_head = (peer->send_queue_head + 1) % PEER_SEND_QUEUE_SIZE;
    peer->send_queue_count--;
  }

  if (peer->socket) {
    plat_socket_close(peer->socket);
    plat_socket_free(peer->socket);
    peer->socket = NULL;
  }
}

/**
 * Check if peer handshake is complete.
 */
echo_bool_t peer_is_ready(const peer_t *peer) {
  return peer && peer->state == PEER_STATE_READY;
}

/**
 * Check if peer is connected (TCP connection established).
 * Does NOT include CONNECTING (async connect in progress) or DISCONNECTED.
 */
echo_bool_t peer_is_connected(const peer_t *peer) {
  return peer && peer->state != PEER_STATE_DISCONNECTED &&
         peer->state != PEER_STATE_CONNECTING;
}

/**
 * Get human-readable state string.
 */
const char *peer_state_string(peer_state_t state) {
  switch (state) {
  case PEER_STATE_DISCONNECTED:
    return "DISCONNECTED";
  case PEER_STATE_CONNECTING:
    return "CONNECTING";
  case PEER_STATE_CONNECTED:
    return "CONNECTED";
  case PEER_STATE_HANDSHAKE_SENT:
    return "HANDSHAKE_SENT";
  case PEER_STATE_HANDSHAKE_RECV:
    return "HANDSHAKE_RECV";
  case PEER_STATE_READY:
    return "READY";
  case PEER_STATE_DISCONNECTING:
    return "DISCONNECTING";
  default:
    return "UNKNOWN";
  }
}

/**
 * Get human-readable disconnect reason string.
 */
const char *peer_disconnect_reason_string(peer_disconnect_reason_t reason) {
  switch (reason) {
  case PEER_DISCONNECT_NONE:
    return "NONE";
  case PEER_DISCONNECT_USER:
    return "USER";
  case PEER_DISCONNECT_PROTOCOL_ERROR:
    return "PROTOCOL_ERROR";
  case PEER_DISCONNECT_TIMEOUT:
    return "TIMEOUT";
  case PEER_DISCONNECT_PEER_CLOSED:
    return "PEER_CLOSED";
  case PEER_DISCONNECT_NETWORK_ERROR:
    return "NETWORK_ERROR";
  case PEER_DISCONNECT_HANDSHAKE_FAIL:
    return "HANDSHAKE_FAIL";
  case PEER_DISCONNECT_MISBEHAVING:
    return "MISBEHAVING";
  default:
    return "UNKNOWN";
  }
}
