/*
 * Bitcoin Echo — Master Configuration
 *
 * All configuration is compile-time. There are no configuration files,
 * no command-line flags, no runtime policy changes. The node behaves
 * identically on every execution.
 *
 * This file coordinates all configuration layers:
 * - Consensus rules (frozen, never change)
 * - Policy settings (configurable, reflect operational choices)
 * - Platform settings (pragmatic, may need updates as systems evolve)
 *
 * To build for a different network, define one of:
 *   ECHO_NETWORK_MAINNET  (default)
 *   ECHO_NETWORK_TESTNET
 *   ECHO_NETWORK_REGTEST
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_CONFIG_H
#define ECHO_CONFIG_H

/*
 * Version information.
 */
#define ECHO_VERSION_MAJOR 0
#define ECHO_VERSION_MINOR 0
#define ECHO_VERSION_PATCH 1
#define ECHO_VERSION_STRING "0.0.1"

/* Protocol version we speak */
#define ECHO_PROTOCOL_VERSION 70016

/* User agent string for version messages */
#define ECHO_USER_AGENT "/BitcoinEcho:0.0.1/"

/*
 * Network selection.
 * Default to mainnet if nothing specified.
 */
#if !defined(ECHO_NETWORK_MAINNET) && !defined(ECHO_NETWORK_TESTNET) &&        \
    !defined(ECHO_NETWORK_REGTEST)
#define ECHO_NETWORK_MAINNET
#endif

/*
 * Network magic bytes.
 * Four bytes that prefix every P2P message to identify the network.
 */
#if defined(ECHO_NETWORK_MAINNET)
#define ECHO_NETWORK_MAGIC 0xD9B4BEF9 /* Little-endian: F9 BE B4 D9 */
#define ECHO_NETWORK_NAME "mainnet"
#elif defined(ECHO_NETWORK_TESTNET)
#define ECHO_NETWORK_MAGIC 0x0709110B /* Little-endian: 0B 11 09 07 */
#define ECHO_NETWORK_NAME "testnet3"
#elif defined(ECHO_NETWORK_REGTEST)
#define ECHO_NETWORK_MAGIC 0xDAB5BFFA /* Little-endian: FA BF B5 DA */
#define ECHO_NETWORK_NAME "regtest"
#endif

/*
 * Network ports.
 */
#if defined(ECHO_NETWORK_MAINNET)
#define ECHO_DEFAULT_PORT 8333
#define ECHO_DEFAULT_RPC_PORT 8332
#elif defined(ECHO_NETWORK_TESTNET)
#define ECHO_DEFAULT_PORT 18333
#define ECHO_DEFAULT_RPC_PORT 18332
#elif defined(ECHO_NETWORK_REGTEST)
#define ECHO_DEFAULT_PORT 18444
#define ECHO_DEFAULT_RPC_PORT 18443
#endif

/*
 * Soft fork activation heights.
 * Network-specific, defined here then used by consensus layer.
 */
#if defined(ECHO_NETWORK_MAINNET)
#define CONSENSUS_BIP16_HEIGHT 173805   /* P2SH */
#define CONSENSUS_BIP34_HEIGHT 227931   /* Height in coinbase */
#define CONSENSUS_BIP65_HEIGHT 388381   /* CHECKLOCKTIMEVERIFY */
#define CONSENSUS_BIP66_HEIGHT 363725   /* Strict DER signatures */
#define CONSENSUS_BIP68_HEIGHT 419328   /* Relative lock-time */
#define CONSENSUS_SEGWIT_HEIGHT 481824  /* Segregated Witness */
#define CONSENSUS_TAPROOT_HEIGHT 709632 /* Taproot/Schnorr */

/*
 * Full Verification: Bitcoin Echo verifies every signature.
 *
 * Unlike Bitcoin Core's AssumeValid optimization, we verify everything.
 * Per the manifesto: "Verify, not believe."
 *
 * Network latency is the bottleneck (~5-7 seconds/block), not CPU.
 * Signature verification adds only ~4-6ms per block. The practical
 * performance impact of full verification is negligible.
 */

#elif defined(ECHO_NETWORK_TESTNET)
#define CONSENSUS_BIP16_HEIGHT 514
#define CONSENSUS_BIP34_HEIGHT 21111
#define CONSENSUS_BIP65_HEIGHT 581885
#define CONSENSUS_BIP66_HEIGHT 330776
#define CONSENSUS_BIP68_HEIGHT 770112
#define CONSENSUS_SEGWIT_HEIGHT 834624
#define CONSENSUS_TAPROOT_HEIGHT 2000000 /* Approximate */

#elif defined(ECHO_NETWORK_REGTEST)
#define CONSENSUS_BIP16_HEIGHT 0 /* Always active */
#define CONSENSUS_BIP34_HEIGHT 500
#define CONSENSUS_BIP65_HEIGHT 1351
#define CONSENSUS_BIP66_HEIGHT 1251
#define CONSENSUS_BIP68_HEIGHT 432
#define CONSENSUS_SEGWIT_HEIGHT 0  /* Always active */
#define CONSENSUS_TAPROOT_HEIGHT 0 /* Always active */

#endif

/*
 * Include configuration layers.
 */
#include "echo_consensus.h"
#include "echo_policy.h"
#include "echo_platform_config.h"

/*
 * Backward compatibility aliases.
 * These maintain compatibility with existing code while we transition
 * to the new layered configuration structure.
 *
 * TODO: Remove these aliases once all code uses new constant names.
 */

/* Consensus aliases (ECHO_* -> CONSENSUS_*) */
#define ECHO_HALVING_INTERVAL CONSENSUS_HALVING_INTERVAL
#define ECHO_INITIAL_SUBSIDY CONSENSUS_INITIAL_SUBSIDY
#define ECHO_DIFFICULTY_INTERVAL CONSENSUS_DIFFICULTY_INTERVAL
#define ECHO_TARGET_BLOCK_TIME CONSENSUS_TARGET_BLOCK_TIME
#define ECHO_TARGET_PERIOD_TIME CONSENSUS_TARGET_PERIOD_TIME
#define ECHO_COINBASE_MATURITY CONSENSUS_COINBASE_MATURITY
#define ECHO_MAX_BLOCK_WEIGHT CONSENSUS_MAX_BLOCK_WEIGHT
#define ECHO_MAX_BLOCK_SIZE CONSENSUS_MAX_BLOCK_SIZE
#define ECHO_MAX_SCRIPT_SIZE CONSENSUS_MAX_SCRIPT_SIZE
#define ECHO_MAX_SCRIPT_ELEMENT CONSENSUS_MAX_SCRIPT_ELEMENT
#define ECHO_MAX_SCRIPT_OPS CONSENSUS_MAX_SCRIPT_OPS
#define ECHO_MAX_STACK_SIZE CONSENSUS_MAX_STACK_SIZE
#define ECHO_MAX_MULTISIG_KEYS CONSENSUS_MAX_MULTISIG_KEYS
#define ECHO_MAX_BLOCK_SIGOPS CONSENSUS_MAX_BLOCK_SIGOPS
#define ECHO_MAX_TX_SIZE CONSENSUS_MAX_TX_SIZE
#define ECHO_LOCKTIME_THRESHOLD CONSENSUS_LOCKTIME_THRESHOLD
#define ECHO_SEQUENCE_FINAL CONSENSUS_SEQUENCE_FINAL
#define ECHO_SEQUENCE_LOCKTIME_DISABLE CONSENSUS_SEQUENCE_LOCKTIME_DISABLE
#define ECHO_SEQUENCE_LOCKTIME_TYPE CONSENSUS_SEQUENCE_LOCKTIME_TYPE
#define ECHO_SEQUENCE_LOCKTIME_MASK CONSENSUS_SEQUENCE_LOCKTIME_MASK
#define ECHO_BIP16_HEIGHT CONSENSUS_BIP16_HEIGHT
#define ECHO_BIP34_HEIGHT CONSENSUS_BIP34_HEIGHT
#define ECHO_BIP65_HEIGHT CONSENSUS_BIP65_HEIGHT
#define ECHO_BIP66_HEIGHT CONSENSUS_BIP66_HEIGHT
#define ECHO_BIP68_HEIGHT CONSENSUS_BIP68_HEIGHT
#define ECHO_SEGWIT_HEIGHT CONSENSUS_SEGWIT_HEIGHT
#define ECHO_TAPROOT_HEIGHT CONSENSUS_TAPROOT_HEIGHT

/* Platform aliases (ECHO_* -> PLATFORM_*) */
#define ECHO_MAX_OUTBOUND_PEERS PLATFORM_MAX_OUTBOUND_PEERS
#define ECHO_MAX_INBOUND_PEERS PLATFORM_MAX_INBOUND_PEERS
#define ECHO_MAX_TOTAL_PEERS PLATFORM_MAX_TOTAL_PEERS
#define ECHO_AUDITION_PEER_COUNT PLATFORM_AUDITION_PEER_COUNT
#define ECHO_CONNECT_TIMEOUT_MS PLATFORM_CONNECT_TIMEOUT_MS
#define ECHO_PING_INTERVAL_MS PLATFORM_PING_INTERVAL_MS
#define ECHO_INACTIVITY_TIMEOUT_MS PLATFORM_INACTIVITY_TIMEOUT_MS
#define ECHO_BLOCKS_DIR PLATFORM_BLOCKS_DIR
#define ECHO_CHAINSTATE_DIR PLATFORM_CHAINSTATE_DIR
#define ECHO_DATABASE_FILE PLATFORM_DATABASE_FILE

#endif /* ECHO_CONFIG_H */
