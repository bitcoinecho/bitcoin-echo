# Bitcoin Echo — POSIX Makefile
# Build once. Build right. Stop.

CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2 -Iinclude -pthread
LDFLAGS = -pthread
TARGET  = echo

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
          src/protocol/mempool.c \
          src/app/node.c \
          src/app/rpc.c \
          src/app/log.c \
          lib/sqlite/sqlite3.c
OBJS    = $(SRCS:.c=.o)

# Test files
TEST_SHA256          = test/unit/test_sha256
TEST_RIPEMD160       = test/unit/test_ripemd160
TEST_SECP256K1_FE    = test/unit/test_secp256k1_fe
TEST_SECP256K1_GROUP = test/unit/test_secp256k1_group
TEST_ECDSA           = test/unit/test_ecdsa
TEST_SCHNORR         = test/unit/test_schnorr
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
TEST_MEMPOOL         = test/unit/test_mempool
TEST_NODE            = test/unit/test_node
TEST_EVENT_LOOP      = test/unit/test_event_loop
TEST_RPC             = test/unit/test_rpc
TEST_LOG             = test/unit/test_log

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Test utility object file
TEST_UTILS_OBJ = test/unit/test_utils.o

test/unit/test_utils.o: test/unit/test_utils.c test/unit/test_utils.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Test targets
$(TEST_SHA256): test/unit/test_sha256.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_RIPEMD160): test/unit/test_ripemd160.c src/crypto/ripemd160.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_SECP256K1_FE): test/unit/test_secp256k1_fe.c src/crypto/secp256k1.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_SECP256K1_GROUP): test/unit/test_secp256k1_group.c src/crypto/secp256k1.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_ECDSA): test/unit/test_ecdsa.c src/crypto/secp256k1.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_SCHNORR): test/unit/test_schnorr.c src/crypto/secp256k1.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_SIG_VERIFY): test/unit/test_sig_verify.c src/consensus/sig_verify.c src/crypto/secp256k1.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_SERIALIZE): test/unit/test_serialize.c src/consensus/serialize.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_TX): test/unit/test_tx.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_BLOCK): test/unit/test_block.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_MERKLE): test/unit/test_merkle.c src/consensus/merkle.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_SCRIPT): test/unit/test_script.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_STACK): test/unit/test_stack.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_OPCODES): test/unit/test_opcodes.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_P2SH): test/unit/test_p2sh.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_TIMELOCK): test/unit/test_timelock.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_SCRIPT_VECTORS): test/unit/test_script_vectors.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/consensus/merkle.c src/consensus/block.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_TX_VALIDATE): test/unit/test_tx_validate.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_BLOCK_VALIDATE): test/unit/test_block_validate.c src/consensus/block_validate.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/consensus/merkle.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_COINBASE): test/unit/test_coinbase.c src/consensus/block_validate.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/consensus/merkle.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_UTXO): test/unit/test_utxo.c src/consensus/utxo.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_CHAINSTATE): test/unit/test_chainstate.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_CONSENSUS): test/unit/test_consensus.c src/consensus/consensus.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block_validate.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/merkle.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_BLOCK_STORAGE): test/unit/test_block_storage.c src/storage/blocks.c src/platform/posix.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_DB): test/unit/test_db.c src/storage/db.c lib/sqlite/sqlite3.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_UTXO_DB): test/unit/test_utxo_db.c src/storage/utxo_db.c src/storage/db.c src/consensus/utxo.c lib/sqlite/sqlite3.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_BLOCK_INDEX_DB): test/unit/test_block_index_db.c src/storage/block_index_db.c src/storage/db.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c lib/sqlite/sqlite3.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_PROTOCOL): test/unit/test_protocol.c src/protocol/messages.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_PROTOCOL_SERIALIZE): test/unit/test_protocol_serialize.c src/protocol/serialize.c src/protocol/messages.c src/consensus/serialize.c src/consensus/block.c src/consensus/tx.c src/crypto/sha256.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_PEER): test/unit/test_peer.c src/protocol/peer.c src/protocol/serialize.c src/protocol/messages.c src/consensus/serialize.c src/consensus/block.c src/consensus/tx.c src/crypto/sha256.c src/platform/posix.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_DISCOVERY): test/unit/test_discovery.c src/protocol/discovery.c src/platform/posix.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_RELAY): test/unit/test_relay.c src/protocol/relay.c src/protocol/peer.c src/protocol/serialize.c src/protocol/messages.c src/consensus/serialize.c src/consensus/block.c src/consensus/tx.c src/crypto/sha256.c src/platform/posix.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_SYNC): test/unit/test_sync.c src/protocol/sync.c src/protocol/peer.c src/protocol/serialize.c src/protocol/messages.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/platform/posix.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_MEMPOOL): test/unit/test_mempool.c src/protocol/mempool.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/utxo.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_NODE): test/unit/test_node.c src/app/node.c src/protocol/mempool.c src/protocol/sync.c src/protocol/discovery.c src/protocol/peer.c src/protocol/relay.c src/protocol/serialize.c src/protocol/messages.c src/consensus/consensus.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block_validate.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/merkle.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/storage/blocks.c src/storage/db.c src/storage/utxo_db.c src/storage/block_index_db.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c src/platform/posix.c lib/sqlite/sqlite3.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_EVENT_LOOP): test/unit/test_event_loop.c src/protocol/mempool.c src/protocol/sync.c src/protocol/discovery.c src/protocol/peer.c src/protocol/relay.c src/protocol/serialize.c src/protocol/messages.c src/consensus/consensus.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block_validate.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/merkle.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/storage/blocks.c src/storage/db.c src/storage/utxo_db.c src/storage/block_index_db.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c src/platform/posix.c lib/sqlite/sqlite3.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_RPC): test/unit/test_rpc.c src/app/rpc.c src/app/node.c src/protocol/mempool.c src/protocol/sync.c src/protocol/discovery.c src/protocol/peer.c src/protocol/relay.c src/protocol/serialize.c src/protocol/messages.c src/consensus/consensus.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block_validate.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/merkle.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/storage/blocks.c src/storage/db.c src/storage/utxo_db.c src/storage/block_index_db.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c src/platform/posix.c lib/sqlite/sqlite3.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_LOG): test/unit/test_log.c src/app/log.c src/platform/posix.c  $(TEST_UTILS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

test: $(TEST_SHA256) $(TEST_RIPEMD160) $(TEST_SECP256K1_FE) $(TEST_SECP256K1_GROUP) $(TEST_ECDSA) $(TEST_SCHNORR) $(TEST_SIG_VERIFY) $(TEST_SERIALIZE) $(TEST_TX) $(TEST_BLOCK) $(TEST_MERKLE) $(TEST_SCRIPT) $(TEST_STACK) $(TEST_OPCODES) $(TEST_P2SH) $(TEST_TIMELOCK) $(TEST_TX_VALIDATE) $(TEST_BLOCK_VALIDATE) $(TEST_COINBASE) $(TEST_UTXO) $(TEST_CHAINSTATE) $(TEST_CONSENSUS) $(TEST_BLOCK_STORAGE) $(TEST_DB) $(TEST_UTXO_DB) $(TEST_BLOCK_INDEX_DB) $(TEST_PROTOCOL) $(TEST_PROTOCOL_SERIALIZE) $(TEST_PEER) $(TEST_DISCOVERY) $(TEST_RELAY) $(TEST_SYNC) $(TEST_MEMPOOL) $(TEST_NODE) $(TEST_EVENT_LOOP) $(TEST_RPC) $(TEST_LOG)
	@./test/run_all_tests.sh

clean:
	rm -f $(TARGET) $(OBJS) $(TEST_SHA256) $(TEST_RIPEMD160) $(TEST_SECP256K1_FE) $(TEST_SECP256K1_GROUP) $(TEST_ECDSA) $(TEST_SCHNORR) $(TEST_SIG_VERIFY) $(TEST_SERIALIZE) $(TEST_TX) $(TEST_BLOCK) $(TEST_MERKLE) $(TEST_SCRIPT) $(TEST_STACK) $(TEST_OPCODES) $(TEST_P2SH) $(TEST_TIMELOCK) $(TEST_SCRIPT_VECTORS) $(TEST_TX_VALIDATE) $(TEST_BLOCK_VALIDATE) $(TEST_COINBASE) $(TEST_UTXO) $(TEST_CHAINSTATE) $(TEST_CONSENSUS) $(TEST_BLOCK_STORAGE) $(TEST_DB) $(TEST_UTXO_DB) $(TEST_BLOCK_INDEX_DB) $(TEST_PROTOCOL) $(TEST_PROTOCOL_SERIALIZE) $(TEST_PEER) $(TEST_DISCOVERY) $(TEST_RELAY) $(TEST_SYNC) $(TEST_MEMPOOL) $(TEST_NODE) $(TEST_EVENT_LOOP) $(TEST_RPC) $(TEST_LOG)
	find src -name '*.o' -delete
	find lib -name '*.o' -delete
