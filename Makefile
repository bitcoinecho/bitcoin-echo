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

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Test targets
$(TEST_SHA256): test/unit/test_sha256.c src/crypto/sha256.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_RIPEMD160): test/unit/test_ripemd160.c src/crypto/ripemd160.c src/crypto/sha256.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_SECP256K1_FE): test/unit/test_secp256k1_fe.c src/crypto/secp256k1.c src/crypto/sha256.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_SECP256K1_GROUP): test/unit/test_secp256k1_group.c src/crypto/secp256k1.c src/crypto/sha256.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_ECDSA): test/unit/test_ecdsa.c src/crypto/secp256k1.c src/crypto/sha256.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_SCHNORR): test/unit/test_schnorr.c src/crypto/secp256k1.c src/crypto/sha256.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_SIG_VERIFY): test/unit/test_sig_verify.c src/consensus/sig_verify.c src/crypto/secp256k1.c src/crypto/sha256.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_SERIALIZE): test/unit/test_serialize.c src/consensus/serialize.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_TX): test/unit/test_tx.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_BLOCK): test/unit/test_block.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_MERKLE): test/unit/test_merkle.c src/consensus/merkle.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_SCRIPT): test/unit/test_script.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_STACK): test/unit/test_stack.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_OPCODES): test/unit/test_opcodes.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_P2SH): test/unit/test_p2sh.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_TIMELOCK): test/unit/test_timelock.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_SCRIPT_VECTORS): test/unit/test_script_vectors.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/consensus/merkle.c src/consensus/block.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_TX_VALIDATE): test/unit/test_tx_validate.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_BLOCK_VALIDATE): test/unit/test_block_validate.c src/consensus/block_validate.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/consensus/merkle.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_COINBASE): test/unit/test_coinbase.c src/consensus/block_validate.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/consensus/merkle.c src/crypto/sha256.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_UTXO): test/unit/test_utxo.c src/consensus/utxo.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_CHAINSTATE): test/unit/test_chainstate.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_CONSENSUS): test/unit/test_consensus.c src/consensus/consensus.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block_validate.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/merkle.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_BLOCK_STORAGE): test/unit/test_block_storage.c src/storage/blocks.c src/platform/posix.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_DB): test/unit/test_db.c src/storage/db.c lib/sqlite/sqlite3.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_UTXO_DB): test/unit/test_utxo_db.c src/storage/utxo_db.c src/storage/db.c src/consensus/utxo.c lib/sqlite/sqlite3.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_BLOCK_INDEX_DB): test/unit/test_block_index_db.c src/storage/block_index_db.c src/storage/db.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c lib/sqlite/sqlite3.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_PROTOCOL): test/unit/test_protocol.c src/protocol/messages.c src/crypto/sha256.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_PROTOCOL_SERIALIZE): test/unit/test_protocol_serialize.c src/protocol/serialize.c src/protocol/messages.c src/consensus/serialize.c src/consensus/block.c src/consensus/tx.c src/crypto/sha256.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_PEER): test/unit/test_peer.c src/protocol/peer.c src/protocol/serialize.c src/protocol/messages.c src/consensus/serialize.c src/consensus/block.c src/consensus/tx.c src/crypto/sha256.c src/platform/posix.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_DISCOVERY): test/unit/test_discovery.c src/protocol/discovery.c src/platform/posix.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_RELAY): test/unit/test_relay.c src/protocol/relay.c src/protocol/peer.c src/protocol/serialize.c src/protocol/messages.c src/consensus/serialize.c src/consensus/block.c src/consensus/tx.c src/crypto/sha256.c src/platform/posix.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_SYNC): test/unit/test_sync.c src/protocol/sync.c src/protocol/peer.c src/protocol/serialize.c src/protocol/messages.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/platform/posix.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_MEMPOOL): test/unit/test_mempool.c src/protocol/mempool.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/utxo.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_NODE): test/unit/test_node.c src/app/node.c src/protocol/mempool.c src/protocol/sync.c src/protocol/discovery.c src/protocol/peer.c src/protocol/relay.c src/protocol/serialize.c src/protocol/messages.c src/consensus/consensus.c src/consensus/chainstate.c src/consensus/utxo.c src/consensus/block_validate.c src/consensus/tx_validate.c src/consensus/script.c src/consensus/sig_verify.c src/consensus/merkle.c src/consensus/block.c src/consensus/tx.c src/consensus/serialize.c src/storage/blocks.c src/storage/db.c src/storage/utxo_db.c src/storage/block_index_db.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c src/platform/posix.c lib/sqlite/sqlite3.c
	$(CC) $(CFLAGS) -o $@ $^

test: $(TEST_SHA256) $(TEST_RIPEMD160) $(TEST_SECP256K1_FE) $(TEST_SECP256K1_GROUP) $(TEST_ECDSA) $(TEST_SCHNORR) $(TEST_SIG_VERIFY) $(TEST_SERIALIZE) $(TEST_TX) $(TEST_BLOCK) $(TEST_MERKLE) $(TEST_SCRIPT) $(TEST_STACK) $(TEST_OPCODES) $(TEST_P2SH) $(TEST_TIMELOCK) $(TEST_TX_VALIDATE) $(TEST_BLOCK_VALIDATE) $(TEST_COINBASE) $(TEST_UTXO) $(TEST_CHAINSTATE) $(TEST_CONSENSUS) $(TEST_BLOCK_STORAGE) $(TEST_DB) $(TEST_UTXO_DB) $(TEST_BLOCK_INDEX_DB) $(TEST_PROTOCOL) $(TEST_PROTOCOL_SERIALIZE) $(TEST_PEER) $(TEST_DISCOVERY) $(TEST_RELAY) $(TEST_SYNC) $(TEST_MEMPOOL) $(TEST_NODE)
	@echo "Running SHA-256 tests..."
	@./$(TEST_SHA256)
	@echo ""
	@echo "Running RIPEMD-160 tests..."
	@./$(TEST_RIPEMD160)
	@echo ""
	@echo "Running secp256k1 field tests..."
	@./$(TEST_SECP256K1_FE)
	@echo ""
	@echo "Running secp256k1 group tests..."
	@./$(TEST_SECP256K1_GROUP)
	@echo ""
	@echo "Running ECDSA tests..."
	@./$(TEST_ECDSA)
	@echo ""
	@echo "Running Schnorr tests..."
	@./$(TEST_SCHNORR)
	@echo ""
	@echo "Running Signature Interface tests..."
	@./$(TEST_SIG_VERIFY)
	@echo ""
	@echo "Running Serialization tests..."
	@./$(TEST_SERIALIZE)
	@echo ""
	@echo "Running Transaction tests..."
	@./$(TEST_TX)
	@echo ""
	@echo "Running Block tests..."
	@./$(TEST_BLOCK)
	@echo ""
	@echo "Running Merkle tree tests..."
	@./$(TEST_MERKLE)
	@echo ""
	@echo "Running Script tests..."
	@./$(TEST_SCRIPT)
	@echo ""
	@echo "Running Stack tests..."
	@./$(TEST_STACK)
	@echo ""
	@echo "Running Opcode tests..."
	@./$(TEST_OPCODES)
	@echo ""
	@echo "Running P2SH tests..."
	@./$(TEST_P2SH)
	@echo ""
	@echo "Running Timelock tests..."
	@./$(TEST_TIMELOCK)
	@echo ""
	@echo "Running Transaction Validation tests..."
	@./$(TEST_TX_VALIDATE)
	@echo ""
	@echo "Running Block Header Validation tests..."
	@./$(TEST_BLOCK_VALIDATE)
	@echo ""
	@echo "Running Coinbase Validation tests..."
	@./$(TEST_COINBASE)
	@echo ""
	@echo "Running UTXO tests..."
	@./$(TEST_UTXO)
	@echo ""
	@echo "Running Chain State tests..."
	@./$(TEST_CHAINSTATE)
	@echo ""
	@echo "Running Consensus Engine tests..."
	@./$(TEST_CONSENSUS)
	@echo ""
	@echo "Running Block Storage tests..."
	@./$(TEST_BLOCK_STORAGE)
	@echo ""
	@echo "Running Database Integration tests..."
	@./$(TEST_DB)
	@echo ""
	@echo "Running UTXO Database tests..."
	@./$(TEST_UTXO_DB)
	@echo ""
	@echo "Running Block Index Database tests..."
	@./$(TEST_BLOCK_INDEX_DB)
	@echo ""
	@echo "Running Protocol Message tests..."
	@./$(TEST_PROTOCOL)
	@echo ""
	@echo "Running Protocol Serialization tests..."
	@./$(TEST_PROTOCOL_SERIALIZE)
	@echo ""
	@echo "Running Peer Management tests..."
	@./$(TEST_PEER)
	@echo ""
	@echo "Running Peer Discovery tests..."
	@./$(TEST_DISCOVERY)
	@echo ""
	@echo "Running Relay tests..."
	@./$(TEST_RELAY)
	@echo ""
	@echo "Running Sync tests..."
	@./$(TEST_SYNC)
	@echo ""
	@echo "Running Mempool tests..."
	@./$(TEST_MEMPOOL)
	@echo ""
	@echo "Running Node Lifecycle tests..."
	@./$(TEST_NODE)

clean:
	rm -f $(TARGET) $(OBJS) $(TEST_SHA256) $(TEST_RIPEMD160) $(TEST_SECP256K1_FE) $(TEST_SECP256K1_GROUP) $(TEST_ECDSA) $(TEST_SCHNORR) $(TEST_SIG_VERIFY) $(TEST_SERIALIZE) $(TEST_TX) $(TEST_BLOCK) $(TEST_MERKLE) $(TEST_SCRIPT) $(TEST_STACK) $(TEST_OPCODES) $(TEST_P2SH) $(TEST_TIMELOCK) $(TEST_SCRIPT_VECTORS) $(TEST_TX_VALIDATE) $(TEST_BLOCK_VALIDATE) $(TEST_COINBASE) $(TEST_UTXO) $(TEST_CHAINSTATE) $(TEST_CONSENSUS) $(TEST_BLOCK_STORAGE) $(TEST_DB) $(TEST_UTXO_DB) $(TEST_BLOCK_INDEX_DB) $(TEST_PROTOCOL) $(TEST_PROTOCOL_SERIALIZE) $(TEST_PEER) $(TEST_DISCOVERY) $(TEST_RELAY) $(TEST_SYNC) $(TEST_MEMPOOL) $(TEST_NODE)
	find src -name '*.o' -delete
	find lib -name '*.o' -delete
