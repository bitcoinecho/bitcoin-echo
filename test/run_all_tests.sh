#!/bin/sh
# Bitcoin Echo — Global Test Runner
# Runs all unit tests and displays grand finale summary
# Build once. Build right. Stop.

# ANSI color codes
BOLD='\033[1m'
GREEN='\033[32m'
RED='\033[31m'
CYAN='\033[36m'
RESET='\033[0m'

# Temporary files for collecting results
RESULTS_FILE=$(mktemp)
COUNTS_FILE=$(mktemp)
TEST_OUTPUT_FILE=$(mktemp)

# Initialize counters file
echo "0 0 0 0 0 0" > "$COUNTS_FILE"

# Test definitions: "executable|description"
run_test() {
    test_exec="$1"
    test_desc="$2"

    # Read current counts
    read total_suites passed_suites failed_suites total_tests passed_tests failed_tests < "$COUNTS_FILE"

    total_suites=$((total_suites + 1))

    echo "Running ${test_desc}..."

    # Run test and capture output
    if ./"$test_exec" > "$TEST_OUTPUT_FILE" 2>&1; then
        exit_code=0
    else
        exit_code=$?
    fi

    # Display the test output
    cat "$TEST_OUTPUT_FILE"
    echo ""

    # Extract test counts from output
    # Look for pattern: "Results: X/Y tests passed"
    suite_passed=$(grep -o '[0-9]\+/[0-9]\+ tests passed' "$TEST_OUTPUT_FILE" | head -1 | cut -d'/' -f1)
    suite_total=$(grep -o '[0-9]\+/[0-9]\+ tests passed' "$TEST_OUTPUT_FILE" | head -1 | cut -d'/' -f2 | cut -d' ' -f1)

    if [ -n "$suite_passed" ] && [ -n "$suite_total" ]; then
        # Record results
        echo "${test_desc}|${suite_passed}|${suite_total}|${exit_code}" >> "$RESULTS_FILE"

        total_tests=$((total_tests + suite_total))
        passed_tests=$((passed_tests + suite_passed))

        if [ "$exit_code" -eq 0 ]; then
            passed_suites=$((passed_suites + 1))
        else
            failed_suites=$((failed_suites + 1))
        fi

        # Write updated counts
        echo "$total_suites $passed_suites $failed_suites $total_tests $passed_tests $failed_tests" > "$COUNTS_FILE"
    fi
}

# Run all tests
run_test "test/unit/test_sha256" "SHA-256 tests"
run_test "test/unit/test_ripemd160" "RIPEMD-160 tests"
run_test "test/unit/test_sig_verify" "Signature Interface tests"
run_test "test/unit/test_serialize" "Serialization tests"
run_test "test/unit/test_tx" "Transaction tests"
run_test "test/unit/test_block" "Block tests"
run_test "test/unit/test_merkle" "Merkle tree tests"
run_test "test/unit/test_script" "Script tests"
run_test "test/unit/test_stack" "Stack tests"
run_test "test/unit/test_opcodes" "Opcode tests"
run_test "test/unit/test_p2sh" "P2SH tests"
run_test "test/unit/test_timelock" "Timelock tests"
run_test "test/unit/test_tx_validate" "Transaction Validation tests"
run_test "test/unit/test_block_validate" "Block Header Validation tests"
run_test "test/unit/test_coinbase" "Coinbase Validation tests"
run_test "test/unit/test_utxo" "UTXO tests"
run_test "test/unit/test_chainstate" "Chain State tests"
run_test "test/unit/test_consensus" "Consensus Engine tests"
run_test "test/unit/test_block_storage" "Block Storage tests"
run_test "test/unit/test_db" "Database Integration tests"
run_test "test/unit/test_utxo_db" "UTXO Database tests"
run_test "test/unit/test_block_index_db" "Block Index Database tests"
run_test "test/unit/test_protocol" "Protocol Message tests"
run_test "test/unit/test_protocol_serialize" "Protocol Serialization tests"
run_test "test/unit/test_peer" "Peer Management tests"
run_test "test/unit/test_discovery" "Peer Discovery tests"
run_test "test/unit/test_relay" "Relay tests"
run_test "test/unit/test_sync" "Sync tests"
run_test "test/unit/test_download_mgr" "Download Manager tests"
run_test "test/unit/test_mempool" "Mempool tests"
run_test "test/unit/test_node" "Node Lifecycle tests"
run_test "test/unit/test_event_loop" "Event Loop tests"
run_test "test/unit/test_rpc" "RPC Interface tests"
run_test "test/unit/test_log" "Logging System tests"
run_test "test/unit/test_pruning" "Pruning tests"
run_test "test/unit/test_mining" "Mining Module tests"
run_test "test/unit/test_integration" "Integration tests"

# Read final counts
read total_suites passed_suites failed_suites total_tests passed_tests failed_tests < "$COUNTS_FILE"

# Calculate failures
failed_tests=$((total_tests - passed_tests))

# Display grand finale
printf "${BOLD}================================================================================\n"
printf "                     BITCOIN ECHO — GLOBAL TEST SUMMARY\n"
printf "================================================================================\n${RESET}\n"

# Display individual suite results
printf "${CYAN}Test Suite Results:${RESET}\n"
while IFS='|' read -r desc passed total exit_code; do
    if [ "$exit_code" -eq 0 ]; then
        printf "  ${GREEN}✓${RESET} %-40s ${GREEN}%3s/%3s passed${RESET}\n" "$desc" "$passed" "$total"
    else
        printf "  ${RED}✗${RESET} %-40s ${RED}%3s/%3s passed${RESET}\n" "$desc" "$passed" "$total"
    fi
done < "$RESULTS_FILE"

printf "\n${BOLD}Summary:${RESET}\n"
printf "  Test Suites: "
if [ "$failed_suites" -eq 0 ]; then
    printf "${GREEN}%d/%d passed${RESET}\n" "$passed_suites" "$total_suites"
else
    printf "${RED}%d/%d passed${RESET}, ${RED}%d failed${RESET}\n" "$passed_suites" "$total_suites" "$failed_suites"
fi

printf "  Test Cases:  "
if [ "$failed_tests" -eq 0 ]; then
    printf "${GREEN}%d/%d passed${RESET}\n" "$passed_tests" "$total_tests"
else
    printf "${RED}%d/%d passed${RESET}, ${RED}%d failed${RESET}\n" "$passed_tests" "$total_tests" "$failed_tests"
fi

printf "\n"

# Grand finale
if [ "$failed_tests" -eq 0 ] && [ "$failed_suites" -eq 0 ]; then
    printf "${BOLD}${GREEN}                    ALL %d TESTS PASSED!${RESET}\n" "$total_tests"
    printf "${BOLD}================================================================================${RESET}\n"
    exit_status=0
else
    printf "${BOLD}${RED}                    %d TEST(S) FAILED${RESET}\n" "$failed_tests"
    printf "${BOLD}================================================================================${RESET}\n"
    exit_status=1
fi

# Cleanup
rm -f "$RESULTS_FILE" "$COUNTS_FILE" "$TEST_OUTPUT_FILE"

exit $exit_status
