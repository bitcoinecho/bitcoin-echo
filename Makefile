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
          src/crypto/ripemd160.c \
          src/crypto/secp256k1.c \
          src/consensus/serialize.c \
          src/consensus/tx.c \
          src/consensus/block.c \
          src/consensus/merkle.c
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

test: $(TEST_SHA256) $(TEST_RIPEMD160) $(TEST_SECP256K1_FE) $(TEST_SECP256K1_GROUP) $(TEST_ECDSA) $(TEST_SCHNORR) $(TEST_SIG_VERIFY) $(TEST_SERIALIZE) $(TEST_TX) $(TEST_BLOCK) $(TEST_MERKLE)
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

clean:
	rm -f $(TARGET) $(OBJS) $(TEST_SHA256) $(TEST_RIPEMD160) $(TEST_SECP256K1_FE) $(TEST_SECP256K1_GROUP) $(TEST_ECDSA) $(TEST_SCHNORR) $(TEST_SIG_VERIFY) $(TEST_SERIALIZE) $(TEST_TX) $(TEST_BLOCK) $(TEST_MERKLE)
	find src -name '*.o' -delete
