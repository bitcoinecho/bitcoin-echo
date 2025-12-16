/*
 * Bitcoin Echo — Platform Configuration
 *
 * These settings control platform-specific behavior: networking, storage,
 * resource limits, and operational parameters. Unlike consensus rules (which
 * must never change) and policy rules (which reflect philosophical choices),
 * platform settings are pragmatic operational decisions.
 *
 * These values may need adjustment as:
 * - Network conditions change
 * - Hardware capabilities evolve
 * - Operating systems update
 *
 * Platform changes do not affect consensus or policy. They affect performance,
 * resource usage, and operational characteristics.
 */

#ifndef ECHO_PLATFORM_CONFIG_H
#define ECHO_PLATFORM_CONFIG_H

/*
 * Peer-to-peer connection limits.
 *
 * Bitcoin nodes maintain connections to other nodes for block/transaction
 * relay. More connections increase redundancy and network health but consume
 * more bandwidth and memory.
 */

/* Maximum outbound connections (connections we initiate) */
#define PLATFORM_MAX_OUTBOUND_PEERS 8

/* Maximum inbound connections (connections others initiate to us) */
#define PLATFORM_MAX_INBOUND_PEERS 117

/* Total maximum connections */
#define PLATFORM_MAX_TOTAL_PEERS                                               \
  (PLATFORM_MAX_OUTBOUND_PEERS + PLATFORM_MAX_INBOUND_PEERS)

/*
 * Network timeouts and intervals (milliseconds).
 *
 * These control how long we wait for responses and how often we perform
 * periodic network maintenance.
 */

/* Connection establishment timeout */
#define PLATFORM_CONNECT_TIMEOUT_MS 5000

/* Ping interval (send keepalive to detect dead connections) */
#define PLATFORM_PING_INTERVAL_MS 120000

/* Inactivity timeout (disconnect if no data received) */
#define PLATFORM_INACTIVITY_TIMEOUT_MS 600000

/*
 * Data directory structure.
 *
 * All blockchain data is stored under a data directory root.
 * These paths are relative to that root.
 */

/* Block data storage directory */
#define PLATFORM_BLOCKS_DIR "blocks"

/* UTXO set and chain state directory */
#define PLATFORM_CHAINSTATE_DIR "chainstate"

/* Main database file */
#define PLATFORM_DATABASE_FILE "echo.db"

/*
 * RPC server configuration.
 *
 * The JSON-RPC interface allows external programs to query blockchain
 * state and submit transactions.
 */

/* Maximum simultaneous RPC connections */
#define PLATFORM_RPC_MAX_CONNECTIONS 16

/* RPC request timeout (milliseconds) */
#define PLATFORM_RPC_TIMEOUT_MS 30000

/*
 * Logging configuration.
 */

/* Log level: 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG */
#define PLATFORM_LOG_LEVEL 2

/* Maximum log file size (bytes) before rotation */
#define PLATFORM_LOG_MAX_SIZE_BYTES 10485760 /* 10 MB */

/* Number of rotated log files to keep */
#define PLATFORM_LOG_ROTATION_COUNT 5

#endif /* ECHO_PLATFORM_CONFIG_H */
