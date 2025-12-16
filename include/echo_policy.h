/*
 * Bitcoin Echo — Policy Configuration
 *
 * These settings control which consensus-valid transactions your node will
 * relay and temporarily store in its mempool. They do NOT affect consensus.
 * All nodes agree on valid blocks regardless of policy settings.
 *
 * Policy settings reflect operational choices:
 * - Resource constraints (bandwidth, storage, CPU)
 * - Philosophical preferences (what Bitcoin should be used for)
 * - Risk tolerance (legal, spam, DoS exposure)
 *
 * Configure these values before compilation based on your requirements.
 * There are no runtime configuration files or command-line flags.
 *
 * Note: Miners may include transactions you filter. Your node will still
 * validate and accept blocks containing filtered transactions because
 * consensus rules determine block validity, not policy rules.
 */

#ifndef ECHO_POLICY_H
#define ECHO_POLICY_H

/*
 * Data carrier (OP_RETURN) policy.
 *
 * OP_RETURN outputs allow embedding arbitrary data in transactions.
 * Consensus permits up to ~10KB per output (limited by max tx size).
 * Policy determines how much data your node will relay.
 *
 * Historical values:
 * - 0 bytes: No OP_RETURN relay (pre-2013)
 * - 40 bytes: Initial OP_RETURN standard (2013-2014)
 * - 80 bytes: Increased standard (2014-2024)
 * - 83 bytes: 80 bytes of data + 3 bytes overhead (actual limit)
 * - 100000 bytes: Effectively unlimited (consensus max applies)
 *
 * Setting to 0 disables OP_RETURN relay entirely.
 * Setting to 100000 accepts up to consensus maximum.
 *
 * Your choice reflects belief about Bitcoin's purpose:
 * - Low values: Prioritize monetary transactions, discourage data storage
 * - High values: Treat all consensus-valid uses as equally legitimate
 */
#define POLICY_MAX_DATACARRIER_BYTES 80

/*
 * Witness data filtering.
 *
 * SegWit witness fields can contain arbitrary data. Some protocols embed
 * images, text, and other non-financial data in witness fields (sometimes
 * called "inscriptions" or "ordinals").
 *
 * 0 = Accept all consensus-valid witness data
 * 1 = Filter transactions with patterns indicating arbitrary data embedding
 *
 * Note: This is pattern matching, not perfect filtering. Sophisticated
 * data embedding may bypass filters. Filtered transactions may still appear
 * in blocks if miners include them.
 *
 * Your choice reflects belief about witness field purpose:
 * - 0: Witness fields are consensus-valid space, any use is legitimate
 * - 1: Witness fields are for signatures/scripts, not data storage
 */
#define POLICY_FILTER_WITNESS_DATA 0

/*
 * Bare multisig relay.
 *
 * Multisig outputs can be "bare" (scriptPubKey directly in output) or
 * "wrapped" (behind P2SH or P2WSH). Bare multisig creates larger UTXO set
 * entries and has been used for data encoding.
 *
 * 0 = Reject bare multisig, only relay P2SH/P2WSH-wrapped multisig
 * 1 = Accept bare multisig outputs
 *
 * Note: Bare multisig remains consensus-valid regardless of this setting.
 * Miners may include bare multisig transactions in blocks.
 *
 * Your choice reflects tradeoff:
 * - 0: Reduce UTXO bloat, discourage data encoding, better privacy
 * - 1: Maximum compatibility, no filtering of valid transaction types
 */
#define POLICY_PERMIT_BARE_MULTISIG 1

/*
 * Minimum relay fee (satoshis per 1000 bytes).
 *
 * Transactions paying less than this fee rate will not be relayed or
 * accepted into mempool. This protects against DoS via free transactions.
 *
 * Note: Miners may mine zero-fee transactions. Blocks containing them
 * are consensus-valid and will be accepted.
 *
 * Typical values:
 * - 1000 satoshis/KB (1 sat/byte): Standard minimum
 * - 0: Accept zero-fee transactions (not recommended, DoS risk)
 * - Higher: More selective relay, less spam exposure
 */
#define POLICY_MIN_RELAY_FEE_RATE 1000

/*
 * Dust threshold (satoshis).
 *
 * Outputs below this value are considered "dust" - worth less than the
 * fee to spend them. Transactions creating dust outputs are not relayed.
 *
 * This prevents UTXO set bloat from economically unspendable outputs.
 *
 * Standard value: 546 satoshis (cost to spend a P2PKH output at 3 sat/byte)
 *
 * Note: Dust outputs remain consensus-valid. Miners may include them.
 */
#define POLICY_DUST_THRESHOLD 546

/*
 * Maximum standard transaction weight (weight units).
 *
 * Transactions heavier than this are considered "non-standard" and will
 * not be relayed, even if consensus-valid.
 *
 * Consensus maximum: 400000 weight units (CONSENSUS_MAX_TX_SIZE)
 * Standard maximum: 400000 weight units (accept up to consensus limit)
 *
 * Lower values reduce DoS risk from large transactions.
 */
#define POLICY_MAX_STANDARD_TX_WEIGHT 400000

/*
 * Maximum signature operations per transaction.
 *
 * Transactions with more signature operations than this limit will not
 * be relayed, even if consensus-valid.
 *
 * This prevents CPU exhaustion from transaction validation.
 *
 * Typical value: 4000 (derived from consensus max / 20)
 */
#define POLICY_MAX_STANDARD_TX_SIGOPS 4000

/*
 * Mempool size limit (megabytes).
 *
 * Maximum memory to allocate for storing unconfirmed transactions.
 * When full, lowest fee-rate transactions are evicted.
 *
 * This is a resource limit, not a consensus parameter.
 *
 * Typical values:
 * - 300 MB: Standard default
 * - Lower: Constrained environments
 * - Higher: More relay capacity, better fee estimation
 */
#define POLICY_MEMPOOL_MAX_SIZE_MB 300

/*
 * Mempool expiry time (hours).
 *
 * Transactions in mempool longer than this are evicted, even if they
 * pay sufficient fees. Prevents mempool bloat from never-mined transactions.
 *
 * Typical value: 336 hours (2 weeks)
 */
#define POLICY_MEMPOOL_EXPIRY_HOURS 336

/*
 * Replace-by-fee (RBF) policy.
 *
 * 0 = Do not relay replacement transactions (first-seen policy)
 * 1 = Relay replacements if they pay higher fee (BIP-125)
 *
 * Note: Both policies are compatible with consensus. This only affects
 * relay behavior and mempool management.
 *
 * Your choice reflects preference:
 * - 0: Faster finality for unconfirmed transactions, simpler logic
 * - 1: Better fee bumping, more flexible for users
 */
#define POLICY_ENABLE_RBF 1

#endif /* ECHO_POLICY_H */
