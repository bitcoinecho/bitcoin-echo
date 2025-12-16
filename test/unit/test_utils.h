/*
 * Bitcoin Echo — Test Utility Framework
 *
 * Provides unified test output formatting with:
 *   - ANSI color support (green PASS, red FAIL)
 *   - Consistent formatting across all test suites
 *   - Detailed failure reporting
 *   - Global test tracking and summary
 *
 * Build once. Build right. Stop.
 */

#ifndef BITCOIN_ECHO_TEST_UTILS_H
#define BITCOIN_ECHO_TEST_UTILS_H

#include <stddef.h>
#include <stdint.h>

/* Compare two byte arrays (returns 1 if equal, 0 otherwise) */
int bytes_equal(const uint8_t *a, const uint8_t *b, size_t len);

/*
 * Test Suite Management
 * =====================
 * Call test_suite_begin() at the start of your test suite,
 * then test_suite_end() at the end to print results.
 */

/* Begin a new test suite with the given name */
void test_suite_begin(const char *suite_name);

/* End the current test suite and print results */
void test_suite_end(void);

/*
 * Test Case Management
 * ====================
 * Each test case should:
 *   1. Call test_case() with a descriptive name
 *   2. Perform the test
 *   3. Call test_pass() or one of the test_fail_*() functions
 */

/* Begin a new test case with the given name */
void test_case(const char *test_name);

/* Mark the current test case as passed */
void test_pass(void);

/* Mark the current test case as failed with a simple message */
void test_fail(const char *message);

/* Mark the current test case as failed with integer mismatch details */
void test_fail_int(const char *message, long expected, long actual);

/* Mark the current test case as failed with unsigned integer mismatch details */
void test_fail_uint(const char *message, unsigned long expected, unsigned long actual);

/* Mark the current test case as failed with string mismatch details */
void test_fail_str(const char *message, const char *expected, const char *actual);

/* Mark the current test case as failed with byte array mismatch details */
void test_fail_bytes(const char *message, const uint8_t *expected, const uint8_t *actual, size_t len);

/*
 * Global Test Summary
 * ===================
 * Call test_global_summary() at the very end of your test runner
 * to print a global summary of all tests run across all suites.
 * Returns 0 if all tests passed, 1 if any failed.
 */

/* Print global summary and return exit code (0 = success, 1 = failure) */
int test_global_summary(void);

/*
 * Section Headers
 * ===============
 * Optional visual separator for grouping related tests within a suite.
 */

/* Print a section header */
void test_section(const char *section_name);

#endif /* BITCOIN_ECHO_TEST_UTILS_H */
