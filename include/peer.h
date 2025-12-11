/**
 * Bitcoin Echo — Peer Connection Management
 *
 * This module manages the lifecycle of P2P connections:
 * - Connection establishment (outbound and inbound)
 * - Handshake (version/verack exchange)
 * - Message sending and receiving
 * - Disconnection handling
 * - Per-peer state tracking
 *
 * Each peer represents one active TCP connection to/from another Bitcoin node.
 */

#ifndef ECHO_PEER_H
#define ECHO_PEER_H

#include "echo_types.h"
#include "platform.h"
#include "protocol.h"

/* Maximum size of per-peer send queue (in messages) */
#define PEER_SEND_QUEUE_SIZE 128

/* Maximum size of per-peer receive buffer (in bytes) */
#define PEER_RECV_BUFFER_SIZE (1024 * 1024)  /* 1 MB */

/**
 * Peer connection state
 */
typedef enum {
    PEER_STATE_DISCONNECTED,     /* Not connected */
    PEER_STATE_CONNECTING,       /* TCP connection in progress */
    PEER_STATE_CONNECTED,        /* TCP connected, handshake not started */
    PEER_STATE_HANDSHAKE_SENT,   /* version message sent, waiting for version */
    PEER_STATE_HANDSHAKE_RECV,   /* version received, waiting for verack */
    PEER_STATE_READY,            /* Handshake complete, ready for messages */
    PEER_STATE_DISCONNECTING     /* Disconnection in progress */
} peer_state_t;

/**
 * Peer disconnect reason
 */
typedef enum {
    PEER_DISCONNECT_NONE = 0,
    PEER_DISCONNECT_USER,           /* User requested disconnect */
    PEER_DISCONNECT_PROTOCOL_ERROR, /* Protocol violation */
    PEER_DISCONNECT_TIMEOUT,        /* Connection timeout */
    PEER_DISCONNECT_PEER_CLOSED,    /* Peer closed connection */
    PEER_DISCONNECT_NETWORK_ERROR,  /* Network I/O error */
    PEER_DISCONNECT_HANDSHAKE_FAIL, /* Handshake failed */
    PEER_DISCONNECT_MISBEHAVING     /* Peer misbehavior (DoS protection) */
} peer_disconnect_reason_t;

/**
 * Message queue entry
 */
typedef struct {
    msg_t message;          /* The message to send */
    echo_bool_t allocated;  /* Whether message contains dynamically allocated data */
} peer_msg_queue_entry_t;

/**
 * Peer connection
 *
 * Represents a single P2P connection to/from another Bitcoin node.
 */
typedef struct {
    /* Connection info */
    plat_socket_t *socket;              /* TCP socket (allocated) */
    peer_state_t state;                 /* Current connection state */
    echo_bool_t inbound;                /* True if peer connected to us */

    /* Peer identification */
    char address[64];                   /* IP address string */
    uint16_t port;                      /* Port number */
    uint64_t nonce_local;               /* Our nonce (for self-connection detection) */
    uint64_t nonce_remote;              /* Peer's nonce */

    /* Protocol version information */
    int32_t version;                    /* Peer's protocol version */
    uint64_t services;                  /* Peer's service flags */
    int32_t start_height;               /* Peer's blockchain height */
    char user_agent[MAX_USER_AGENT_LEN]; /* Peer's user agent string */
    size_t user_agent_len;              /* Actual length of user_agent */
    echo_bool_t relay;                  /* Whether peer wants tx relay */

    /* Connection timing */
    uint64_t connect_time;              /* When connection established (plat_time_ms) */
    uint64_t last_send;                 /* Last successful send time */
    uint64_t last_recv;                 /* Last successful receive time */

    /* Send queue */
    peer_msg_queue_entry_t send_queue[PEER_SEND_QUEUE_SIZE];
    size_t send_queue_head;             /* Next message to send */
    size_t send_queue_tail;             /* Where to add new messages */
    size_t send_queue_count;            /* Number of queued messages */

    /* Receive buffer */
    uint8_t recv_buffer[PEER_RECV_BUFFER_SIZE];
    size_t recv_buffer_len;             /* Bytes currently in buffer */

    /* Disconnection info */
    peer_disconnect_reason_t disconnect_reason;
    char disconnect_message[256];       /* Human-readable disconnect reason */

    /* Statistics */
    uint64_t bytes_sent;
    uint64_t bytes_recv;
    uint64_t messages_sent;
    uint64_t messages_recv;
} peer_t;

/**
 * Initialize peer structure.
 *
 * Must be called before using a peer_t.
 */
void peer_init(peer_t *peer);

/**
 * Create outbound connection to remote peer.
 *
 * Establishes TCP connection and transitions peer to PEER_STATE_CONNECTED.
 * Does NOT send version message — call peer_send_version() after this.
 *
 * Parameters:
 *   peer    - Peer structure to initialize
 *   address - IP address to connect to
 *   port    - Port number
 *   nonce   - Random nonce for self-connection detection
 *
 * Returns:
 *   ECHO_SUCCESS on successful connection
 *   ECHO_ERR_NETWORK on connection failure
 */
echo_result_t peer_connect(peer_t *peer, const char *address, uint16_t port, uint64_t nonce);

/**
 * Accept inbound connection from listening socket.
 *
 * Accepts connection and transitions peer to PEER_STATE_CONNECTED.
 * Does NOT send version message — call peer_send_version() after this.
 *
 * Parameters:
 *   peer     - Peer structure to initialize
 *   listener - Listening socket
 *   nonce    - Random nonce for self-connection detection
 *
 * Returns:
 *   ECHO_SUCCESS on successful accept
 *   ECHO_ERR_NETWORK on failure
 */
echo_result_t peer_accept(peer_t *peer, plat_socket_t *listener, uint64_t nonce);

/**
 * Send version message to peer.
 *
 * Initiates handshake. Peer must be in PEER_STATE_CONNECTED.
 * Transitions peer to PEER_STATE_HANDSHAKE_SENT.
 *
 * Parameters:
 *   peer          - Peer to send version to
 *   our_services  - Our service flags
 *   our_height    - Our blockchain height
 *   relay         - Whether we want transaction relay
 *
 * Returns:
 *   ECHO_SUCCESS on successful send
 *   ECHO_ERR_NETWORK on send failure
 *   ECHO_ERR_INVALID_STATE if peer not in correct state
 */
echo_result_t peer_send_version(peer_t *peer, uint64_t our_services,
                                int32_t our_height, echo_bool_t relay);

/**
 * Process incoming data from peer.
 *
 * Reads from socket, parses messages, handles handshake.
 * Should be called when socket is ready for reading.
 *
 * Parameters:
 *   peer - Peer to receive from
 *   msg  - Output: received message (if any)
 *
 * Returns:
 *   ECHO_SUCCESS if message received (*msg is valid)
 *   ECHO_ERR_WOULD_BLOCK if no complete message available (not an error)
 *   ECHO_ERR_NETWORK on network error (peer should be disconnected)
 *   ECHO_ERR_PROTOCOL on protocol violation (peer should be disconnected)
 */
echo_result_t peer_receive(peer_t *peer, msg_t *msg);

/**
 * Queue message for sending to peer.
 *
 * Adds message to send queue. Actual sending happens in peer_send_queued().
 *
 * Parameters:
 *   peer - Peer to send to
 *   msg  - Message to queue (will be copied)
 *
 * Returns:
 *   ECHO_SUCCESS if queued
 *   ECHO_ERR_FULL if send queue is full
 *   ECHO_ERR_INVALID_STATE if peer not ready
 */
echo_result_t peer_queue_message(peer_t *peer, const msg_t *msg);

/**
 * Send queued messages to peer.
 *
 * Attempts to send messages from queue.
 * Should be called when socket is ready for writing.
 *
 * Parameters:
 *   peer - Peer to send to
 *
 * Returns:
 *   ECHO_SUCCESS if messages sent (or queue empty)
 *   ECHO_ERR_NETWORK on network error (peer should be disconnected)
 */
echo_result_t peer_send_queued(peer_t *peer);

/**
 * Disconnect peer.
 *
 * Closes connection and transitions to PEER_STATE_DISCONNECTED.
 *
 * Parameters:
 *   peer    - Peer to disconnect
 *   reason  - Disconnect reason code
 *   message - Human-readable reason (optional, can be NULL)
 */
void peer_disconnect(peer_t *peer, peer_disconnect_reason_t reason, const char *message);

/**
 * Check if peer handshake is complete.
 *
 * Returns:
 *   true if peer is in PEER_STATE_READY
 */
echo_bool_t peer_is_ready(const peer_t *peer);

/**
 * Check if peer is connected (in any state except DISCONNECTED).
 *
 * Returns:
 *   true if peer has active connection
 */
echo_bool_t peer_is_connected(const peer_t *peer);

/**
 * Get human-readable string for peer state.
 *
 * Returns:
 *   String representation of state (e.g., "READY")
 */
const char *peer_state_string(peer_state_t state);

/**
 * Get human-readable string for disconnect reason.
 *
 * Returns:
 *   String representation of reason (e.g., "PROTOCOL_ERROR")
 */
const char *peer_disconnect_reason_string(peer_disconnect_reason_t reason);

#endif /* ECHO_PEER_H */
