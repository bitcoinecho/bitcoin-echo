# Bitcoin Echo — POSIX Makefile
# Build once. Build right. Stop.

CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2 -g -Iinclude -pthread
LDFLAGS = -pthread -lm
TARGET  = echo

# libsecp256k1 configuration
# We compile the vendored libsecp256k1 with Schnorr support enabled
LIBSECP_CFLAGS = -DENABLE_MODULE_EXTRAKEYS -DENABLE_MODULE_SCHNORRSIG \
                 -Ilib/secp256k1/include -Ilib/secp256k1/src \
                 -Wno-unused-function -Wno-pedantic -w
LIBSECP_SRCS   = lib/secp256k1/src/secp256k1.c \
                 lib/secp256k1/src/precomputed_ecmult.c \
                 lib/secp256k1/src/precomputed_ecmult_gen.c \
                 lib/secp256k1/contrib/lax_der_parsing.c
LIBSECP_OBJS   = $(LIBSECP_SRCS:.c=.o)

# Source files (will be populated as implementation progresses)
SRCS    = src/main.c \
          src/platform/posix.c \
          src/crypto/sha256.c \
          src/crypto/sha1.c \
          src/crypto/ripemd160.c \
          src/crypto/secp256k1.c \
          src/consensus/serialize.c \
          src/consensus/tx.c \
          src/consensus/block.c \
          src/consensus/merkle.c \
          src/consensus/script.c \
          src/consensus/sig_verify.c \
          src/consensus/tx_validate.c \
          src/consensus/block_validate.c \
          src/consensus/utxo.c \
          src/consensus/chainstate.c \
          src/consensus/consensus.c \
          src/storage/blocks.c \
          src/storage/db.c \
          src/storage/utxo_db.c \
          src/storage/block_index_db.c \
          src/protocol/messages.c \
          src/protocol/serialize.c \
          src/protocol/peer.c \
          src/protocol/discovery.c \
          src/protocol/relay.c \
          src/protocol/sync.c \
          src/protocol/download_mgr.c \
          src/protocol/mempool.c \
          src/app/node.c \
          src/app/rpc.c \
          src/app/log.c \
          src/app/mining.c \
          src/node/chase.c \
          src/node/chaser.c \
          src/node/chaser_validate.c \
          src/node/chaser_confirm.c \
          lib/sqlite/sqlite3.c
OBJS    = $(SRCS:.c=.o)

# Test files
TEST_SHA256          = test/unit/test_sha256
TEST_RIPEMD160       = test/unit/test_ripemd160
TEST_SIG_VERIFY      = test/unit/test_sig_verify
TEST_SERIALIZE       = test/unit/test_serialize
TEST_TX              = test/unit/test_tx
TEST_BLOCK           = test/unit/test_block
TEST_MERKLE          = test/unit/test_merkle
TEST_SCRIPT          = test/unit/test_script
TEST_STACK           = test/unit/test_stack
TEST_OPCODES         = test/unit/test_opcodes
TEST_P2SH            = test/unit/test_p2sh
TEST_TIMELOCK        = test/unit/test_timelock
TEST_SCRIPT_VECTORS  = test/unit/test_script_vectors
TEST_TX_VALIDATE     = test/unit/test_tx_validate
TEST_BLOCK_VALIDATE  = test/unit/test_block_validate
TEST_COINBASE        = test/unit/test_coinbase
TEST_UTXO            = test/unit/test_utxo
TEST_CHAINSTATE      = test/unit/test_chainstate
TEST_CONSENSUS       = test/unit/test_consensus
TEST_BLOCK_STORAGE   = test/unit/test_block_storage
TEST_DB              = test/unit/test_db
TEST_UTXO_DB         = test/unit/test_utxo_db
TEST_BLOCK_INDEX_DB  = test/unit/test_block_index_db
TEST_PROTOCOL        = test/unit/test_protocol
TEST_PROTOCOL_SERIALIZE = test/unit/test_protocol_serialize
TEST_PEER            = test/unit/test_peer
TEST_DISCOVERY       = test/unit/test_discovery
TEST_RELAY           = test/unit/test_relay
TEST_SYNC            = test/unit/test_sync
TEST_DOWNLOAD_MGR    = test/unit/test_download_mgr
TEST_MEMPOOL         = test/unit/test_mempool
TEST_NODE            = test/unit/test_node
TEST_EVENT_LOOP      = test/unit/test_event_loop
TEST_RPC             = test/unit/test_rpc
TEST_LOG             = test/unit/test_log
TEST_PRUNING         = test/unit/test_pruning
TEST_MINING          = test/unit/test_mining
TEST_INTEGRATION     = test/unit/test_integration
TEST_CHASE           = test/unit/test_chase

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS) $(LIBSECP_OBJS)
	$(CC) -o $@ $(OBJS) $(LIBSECP_OBJS) $(LDFLAGS)

# Default rule for our source files
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# libsecp256k1 compilation rules
lib/secp256k1/src/secp256k1.o: lib/secp256k1/src/secp256k1.c
	$(CC) -std=c11 -O2 $(LIBSECP_CFLAGS) -c -o $@ $<

lib/secp256k1/src/precomputed_ecmult.o: lib/secp256k1/src/precomputed_ecmult.c
	$(CC) -std=c11 -O2 $(LIBSECP_CFLAGS) -c -o $@ $<

lib/secp256k1/src/precomputed_ecmult_gen.o: lib/secp256k1/src/precomputed_ecmult_gen.c
	$(CC) -std=c11 -O2 $(LIBSECP_CFLAGS) -c -o $@ $<

lib/secp256k1/contrib/lax_der_parsing.o: lib/secp256k1/contrib/lax_der_parsing.c
	$(CC) -std=c11 -O2 $(LIBSECP_CFLAGS) -c -o $@ $<

# sig_verify.c directly includes libsecp256k1 headers
src/consensus/sig_verify.o: src/consensus/sig_verify.c
	$(CC) $(CFLAGS) $(LIBSECP_CFLAGS) -c -o $@ $<

# script.c directly includes libsecp256k1 headers for Taproot tweak verification
src/consensus/script.o: src/consensus/script.c
	$(CC) $(CFLAGS) $(LIBSECP_CFLAGS) -c -o $@ $<

# Test utility object file
TEST_UTILS_OBJ = test/unit/test_utils.o

test/unit/test_utils.o: test/unit/test_utils.c test/unit/test_utils.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Test targets
$(TEST_SHA256): test/unit/test_sha256.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_RIPEMD160): test/unit/test_ripemd160.c src/crypto/ripemd160.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_SIG_VERIFY): test/unit/test_sig_verify.c src/consensus/sig_verify.c src/crypto/secp256k1.c src/crypto/sha256.c $(TEST_UTILS_OBJ) $(LIBSECP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_SERIALIZE): test/unit/test_serialize.c src/consensus/serialize.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_TX): test/unit/test_tx.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_BLOCK): test/unit/test_block.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_MERKLE): test/unit/test_merkle.c src/consensus/merkle.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_SCRIPT): test/unit/test_script.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c $(TEST_UTILS_OBJ) $(LIBSECP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_STACK): test/unit/test_stack.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c $(TEST_UTILS_OBJ) $(LIBSECP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_OPCODES): test/unit/test_opcodes.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c $(TEST_UTILS_OBJ) $(LIBSECP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_P2SH): test/unit/test_p2sh.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c $(TEST_UTILS_OBJ) $(LIBSECP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_TIMELOCK): test/unit/test_timelock.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c $(TEST_UTILS_OBJ) $(LIBSECP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_SCRIPT_VECTORS): test/unit/test_script_vectors.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/consensus/merkle.c src/consensus/block.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c $(TEST_UTILS_OBJ) $(LIBSECP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_TX_VALIDATE): test/unit/test_tx_validate.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c $(TEST_UTILS_OBJ) $(LIBSECP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_BLOCK_VALIDATE): test/unit/test_block_validate.c src/consensus/block_validate.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/consensus/merkle.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_COINBASE): test/unit/test_coinbase.c src/consensus/block_validate.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/consensus/merkle.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_UTXO): test/unit/test_utxo.c src/consensus/utxo.c src/platform/posix.c src/app/log.c $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_CHAINSTATE): test/unit/test_chainstate.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/platform/posix.c src/app/log.c $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_CONSENSUS): test/unit/test_consensus.c src/consensus/consensus.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block_validate.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/merkle.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c src/platform/posix.c src/app/log.c $(TEST_UTILS_OBJ) $(LIBSECP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_BLOCK_STORAGE): test/unit/test_block_storage.c src/storage/blocks.c src/platform/posix.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_DB): test/unit/test_db.c src/storage/db.c lib/sqlite/sqlite3.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_UTXO_DB): test/unit/test_utxo_db.c src/storage/utxo_db.c src/storage/db.c src/consensus/utxo.c lib/sqlite/sqlite3.c src/platform/posix.c src/app/log.c $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_BLOCK_INDEX_DB): test/unit/test_block_index_db.c src/storage/block_index_db.c src/storage/db.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c lib/sqlite/sqlite3.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_PROTOCOL): test/unit/test_protocol.c src/protocol/messages.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_PROTOCOL_SERIALIZE): test/unit/test_protocol_serialize.c src/protocol/serialize.c src/protocol/messages.c src/consensus/serialize.c src/consensus/block.c src/consensus/tx.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_PEER): test/unit/test_peer.c src/protocol/peer.c src/protocol/serialize.c src/protocol/messages.c src/consensus/serialize.c src/consensus/block.c src/consensus/tx.c src/crypto/sha256.c src/platform/posix.c src/app/log.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_DISCOVERY): test/unit/test_discovery.c src/protocol/discovery.c src/platform/posix.c src/app/log.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_RELAY): test/unit/test_relay.c src/protocol/relay.c src/protocol/peer.c src/protocol/serialize.c src/protocol/messages.c src/consensus/serialize.c src/consensus/block.c src/consensus/tx.c src/crypto/sha256.c src/platform/posix.c src/app/log.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_SYNC): test/unit/test_sync.c src/protocol/sync.c src/protocol/download_mgr.c src/protocol/peer.c src/protocol/serialize.c src/protocol/messages.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/platform/posix.c src/app/log.c src/node/chase.c $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_DOWNLOAD_MGR): test/unit/test_download_mgr.c src/protocol/download_mgr.c src/platform/posix.c src/app/log.c $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_MEMPOOL): test/unit/test_mempool.c src/protocol/mempool.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/utxo.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c src/platform/posix.c src/app/log.c $(TEST_UTILS_OBJ) $(LIBSECP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_NODE): test/unit/test_node.c src/app/node.c src/app/log.c src/protocol/mempool.c src/protocol/sync.c src/protocol/download_mgr.c src/protocol/discovery.c src/protocol/peer.c src/protocol/relay.c src/protocol/serialize.c src/protocol/messages.c src/consensus/consensus.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block_validate.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/merkle.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/storage/blocks.c src/storage/db.c src/storage/utxo_db.c src/storage/block_index_db.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c src/platform/posix.c src/node/chase.c src/node/chaser.c src/node/chaser_validate.c src/node/chaser_confirm.c lib/sqlite/sqlite3.c $(TEST_UTILS_OBJ) $(LIBSECP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_EVENT_LOOP): test/unit/test_event_loop.c src/app/log.c src/protocol/mempool.c src/protocol/sync.c src/protocol/download_mgr.c src/protocol/discovery.c src/protocol/peer.c src/protocol/relay.c src/protocol/serialize.c src/protocol/messages.c src/consensus/consensus.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block_validate.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/merkle.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/storage/blocks.c src/storage/db.c src/storage/utxo_db.c src/storage/block_index_db.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c src/platform/posix.c src/node/chase.c src/node/chaser.c src/node/chaser_validate.c src/node/chaser_confirm.c lib/sqlite/sqlite3.c $(TEST_UTILS_OBJ) $(LIBSECP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_RPC): test/unit/test_rpc.c src/app/rpc.c src/app/node.c src/app/log.c src/protocol/mempool.c src/protocol/sync.c src/protocol/download_mgr.c src/protocol/discovery.c src/protocol/peer.c src/protocol/relay.c src/protocol/serialize.c src/protocol/messages.c src/consensus/consensus.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block_validate.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/merkle.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/storage/blocks.c src/storage/db.c src/storage/utxo_db.c src/storage/block_index_db.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c src/platform/posix.c src/node/chase.c src/node/chaser.c src/node/chaser_validate.c src/node/chaser_confirm.c lib/sqlite/sqlite3.c $(TEST_UTILS_OBJ) $(LIBSECP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_LOG): test/unit/test_log.c src/app/log.c src/platform/posix.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_PRUNING): test/unit/test_pruning.c src/storage/blocks.c src/storage/block_index_db.c src/storage/db.c src/app/node.c src/app/log.c src/protocol/mempool.c src/protocol/sync.c src/protocol/download_mgr.c src/protocol/discovery.c src/protocol/peer.c src/protocol/relay.c src/protocol/serialize.c src/protocol/messages.c src/consensus/consensus.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block_validate.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/merkle.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/storage/utxo_db.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c src/platform/posix.c src/node/chase.c src/node/chaser.c src/node/chaser_validate.c src/node/chaser_confirm.c lib/sqlite/sqlite3.c $(TEST_UTILS_OBJ) $(LIBSECP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_MINING): test/unit/test_mining.c src/app/mining.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_INTEGRATION): test/unit/test_integration.c src/app/node.c src/app/log.c src/app/mining.c src/protocol/mempool.c src/protocol/sync.c src/protocol/download_mgr.c src/protocol/discovery.c src/protocol/peer.c src/protocol/relay.c src/protocol/serialize.c src/protocol/messages.c src/consensus/consensus.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block_validate.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/merkle.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/storage/blocks.c src/storage/db.c src/storage/utxo_db.c src/storage/block_index_db.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c src/platform/posix.c src/node/chase.c src/node/chaser.c src/node/chaser_validate.c src/node/chaser_confirm.c lib/sqlite/sqlite3.c $(TEST_UTILS_OBJ) $(LIBSECP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_CHASE): test/unit/test_chase.c src/node/chase.c src/node/chaser.c src/node/chaser_validate.c src/node/chaser_confirm.c src/platform/posix.c $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: $(TEST_SHA256) $(TEST_RIPEMD160) $(TEST_SIG_VERIFY) $(TEST_SERIALIZE) $(TEST_TX) $(TEST_BLOCK) $(TEST_MERKLE) $(TEST_SCRIPT) $(TEST_STACK) $(TEST_OPCODES) $(TEST_P2SH) $(TEST_TIMELOCK) $(TEST_TX_VALIDATE) $(TEST_BLOCK_VALIDATE) $(TEST_COINBASE) $(TEST_UTXO) $(TEST_CHAINSTATE) $(TEST_CONSENSUS) $(TEST_BLOCK_STORAGE) $(TEST_DB) $(TEST_UTXO_DB) $(TEST_BLOCK_INDEX_DB) $(TEST_PROTOCOL) $(TEST_PROTOCOL_SERIALIZE) $(TEST_PEER) $(TEST_DISCOVERY) $(TEST_RELAY) $(TEST_SYNC) $(TEST_DOWNLOAD_MGR) $(TEST_MEMPOOL) $(TEST_NODE) $(TEST_EVENT_LOOP) $(TEST_RPC) $(TEST_LOG) $(TEST_PRUNING) $(TEST_MINING) $(TEST_INTEGRATION) $(TEST_CHASE)
	@./test/run_all_tests.sh

clean:
	rm -f $(TARGET) $(OBJS) $(LIBSECP_OBJS) $(TEST_SHA256) $(TEST_RIPEMD160) $(TEST_SIG_VERIFY) $(TEST_SERIALIZE) $(TEST_TX) $(TEST_BLOCK) $(TEST_MERKLE) $(TEST_SCRIPT) $(TEST_STACK) $(TEST_OPCODES) $(TEST_P2SH) $(TEST_TIMELOCK) $(TEST_SCRIPT_VECTORS) $(TEST_TX_VALIDATE) $(TEST_BLOCK_VALIDATE) $(TEST_COINBASE) $(TEST_UTXO) $(TEST_CHAINSTATE) $(TEST_CONSENSUS) $(TEST_BLOCK_STORAGE) $(TEST_DB) $(TEST_UTXO_DB) $(TEST_BLOCK_INDEX_DB) $(TEST_PROTOCOL) $(TEST_PROTOCOL_SERIALIZE) $(TEST_PEER) $(TEST_DISCOVERY) $(TEST_RELAY) $(TEST_SYNC) $(TEST_DOWNLOAD_MGR) $(TEST_MEMPOOL) $(TEST_NODE) $(TEST_EVENT_LOOP) $(TEST_RPC) $(TEST_LOG) $(TEST_PRUNING) $(TEST_MINING) $(TEST_INTEGRATION) $(TEST_CHASE)
	find src -name '*.o' -delete
	find lib -name '*.o' -delete
