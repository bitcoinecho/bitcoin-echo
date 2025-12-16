/*
 * Bitcoin Echo — Consensus Configuration
 *
 * These parameters define Bitcoin's consensus rules. They are FROZEN and must
 * never be changed. Any modification to these values creates a hard fork.
 *
 * These rules determine:
 * - What makes a block valid
 * - What makes a transaction valid
 * - How difficulty adjusts
 * - When soft forks activate
 *
 * All Bitcoin Echo nodes, regardless of policy configuration, use identical
 * consensus rules and will therefore agree on the valid chain.
 */

#ifndef ECHO_CONSENSUS_H
#define ECHO_CONSENSUS_H

/*
 * Block subsidy and halving.
 */

/* Initial block subsidy in satoshis (50 BTC) */
#define CONSENSUS_INITIAL_SUBSIDY 5000000000LL

/* Block subsidy halving interval (blocks) */
#define CONSENSUS_HALVING_INTERVAL 210000

/*
 * Difficulty adjustment.
 */

/* Difficulty adjustment interval (blocks) */
#define CONSENSUS_DIFFICULTY_INTERVAL 2016

/* Target time per block (seconds) */
#define CONSENSUS_TARGET_BLOCK_TIME 600

/* Target time per difficulty period (seconds) */
#define CONSENSUS_TARGET_PERIOD_TIME                                           \
  (CONSENSUS_DIFFICULTY_INTERVAL * CONSENSUS_TARGET_BLOCK_TIME)

/*
 * Transaction and block size limits.
 */

/* Maximum block weight (weight units, post-SegWit) */
#define CONSENSUS_MAX_BLOCK_WEIGHT 4000000

/* Maximum block size for legacy calculations (bytes, pre-SegWit) */
#define CONSENSUS_MAX_BLOCK_SIZE 1000000

/* Maximum transaction size (bytes) */
#define CONSENSUS_MAX_TX_SIZE 400000

/*
 * Script limits.
 */

/* Maximum script size (bytes) */
#define CONSENSUS_MAX_SCRIPT_SIZE 10000

/* Maximum script element size (bytes) */
#define CONSENSUS_MAX_SCRIPT_ELEMENT 520

/* Maximum number of operations per script */
#define CONSENSUS_MAX_SCRIPT_OPS 201

/* Maximum stack size during script execution */
#define CONSENSUS_MAX_STACK_SIZE 1000

/* Maximum number of public keys in multisig */
#define CONSENSUS_MAX_MULTISIG_KEYS 20

/* Maximum number of signature operations per block */
#define CONSENSUS_MAX_BLOCK_SIGOPS 80000

/*
 * Timelock parameters.
 */

/* Coinbase maturity (blocks before spendable) */
#define CONSENSUS_COINBASE_MATURITY 100

/* Locktime threshold: values below are block heights, above are timestamps */
#define CONSENSUS_LOCKTIME_THRESHOLD 500000000

/* Sequence number that disables locktime */
#define CONSENSUS_SEQUENCE_FINAL 0xFFFFFFFF

/* BIP-68 sequence lock flags */
#define CONSENSUS_SEQUENCE_LOCKTIME_DISABLE (1U << 31)
#define CONSENSUS_SEQUENCE_LOCKTIME_TYPE (1U << 22)
#define CONSENSUS_SEQUENCE_LOCKTIME_MASK 0x0000FFFF

/*
 * Soft fork activation heights.
 *
 * After these block heights, the corresponding consensus rules are enforced.
 * Before these heights, the rules are not active.
 *
 * These are network-specific and defined in echo_config.h based on
 * ECHO_NETWORK_MAINNET, ECHO_NETWORK_TESTNET, or ECHO_NETWORK_REGTEST.
 */

/* Network-specific activation heights are defined in echo_config.h:
 * - CONSENSUS_BIP16_HEIGHT   (P2SH)
 * - CONSENSUS_BIP34_HEIGHT   (Height in coinbase)
 * - CONSENSUS_BIP65_HEIGHT   (CHECKLOCKTIMEVERIFY)
 * - CONSENSUS_BIP66_HEIGHT   (Strict DER signatures)
 * - CONSENSUS_BIP68_HEIGHT   (Relative lock-time)
 * - CONSENSUS_SEGWIT_HEIGHT  (Segregated Witness)
 * - CONSENSUS_TAPROOT_HEIGHT (Taproot/Schnorr)
 */

#endif /* ECHO_CONSENSUS_H */
