/*
 * Bitcoin Echo — Test Utility Framework Implementation
 *
 * Build once. Build right. Stop.
 */

#include "test_utils.h"
#include <stdio.h>
#include <string.h>

/*
 * ANSI Color Codes
 * ================
 * Terminal colors for beautiful test output.
 */
#define COLOR_RESET   "\033[0m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_RED     "\033[31m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BOLD    "\033[1m"

/*
 * Test State Tracking
 * ===================
 * Track current suite and global statistics.
 */

/* Current suite state */
static const char *current_suite = NULL;
static int suite_tests_run = 0;
static int suite_tests_passed = 0;

/* Global state (across all suites) */
static int global_tests_run = 0;
static int global_tests_passed = 0;
static int global_suites_run = 0;
static int global_suites_passed = 0;

/* Current test case name (for deferred output) */
static const char *current_test = NULL;

/*
 * Helper Functions
 * ================
 */

/* Print bytes as hex (for failure output) */
static void print_hex(const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
}

/*
 * Compare two byte arrays and return 1 if equal, 0 otherwise.
 */
int bytes_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/*
 * Public API Implementation
 * ==========================
 */

void test_suite_begin(const char *suite_name)
{
    current_suite = suite_name;
    suite_tests_run = 0;
    suite_tests_passed = 0;
    global_suites_run++;

    printf("%s%s%s\n", COLOR_BOLD, suite_name, COLOR_RESET);

    /* Print separator of equal length to suite name */
    size_t i;
    size_t len = strlen(suite_name);
    for (i = 0; i < len; i++) {
        printf("=");
    }
    printf("\n\n");
}

void test_suite_end(void)
{
    if (current_suite == NULL) {
        return;
    }

    /* Update global counters */
    global_tests_run += suite_tests_run;
    global_tests_passed += suite_tests_passed;

    if (suite_tests_passed == suite_tests_run) {
        global_suites_passed++;
    }

    /* Print suite results */
    printf("\n");
    if (suite_tests_passed == suite_tests_run) {
        printf("%sResults: %d/%d tests passed%s\n",
               COLOR_GREEN, suite_tests_passed, suite_tests_run, COLOR_RESET);
    } else {
        printf("%sResults: %d/%d tests passed (%d failed)%s\n",
               COLOR_RED, suite_tests_passed, suite_tests_run,
               suite_tests_run - suite_tests_passed, COLOR_RESET);
    }
    printf("\n");

    current_suite = NULL;
}

void test_case(const char *test_name)
{
    current_test = test_name;
    suite_tests_run++;
}

void test_pass(void)
{
    if (current_test == NULL) {
        return;
    }

    suite_tests_passed++;
    printf("  %s[PASS]%s %s\n", COLOR_GREEN, COLOR_RESET, current_test);
    current_test = NULL;
}

void test_fail(const char *message)
{
    if (current_test == NULL) {
        return;
    }

    printf("  %s[FAIL]%s %s\n", COLOR_RED, COLOR_RESET, current_test);
    if (message != NULL && message[0] != '\0') {
        printf("    %s%s%s\n", COLOR_YELLOW, message, COLOR_RESET);
    }
    current_test = NULL;
}

void test_fail_int(const char *message, long expected, long actual)
{
    if (current_test == NULL) {
        return;
    }

    printf("  %s[FAIL]%s %s\n", COLOR_RED, COLOR_RESET, current_test);
    if (message != NULL && message[0] != '\0') {
        printf("    %s%s%s\n", COLOR_YELLOW, message, COLOR_RESET);
    }
    printf("    Expected: %ld\n", expected);
    printf("    Got:      %ld\n", actual);
    current_test = NULL;
}

void test_fail_uint(const char *message, unsigned long expected, unsigned long actual)
{
    if (current_test == NULL) {
        return;
    }

    printf("  %s[FAIL]%s %s\n", COLOR_RED, COLOR_RESET, current_test);
    if (message != NULL && message[0] != '\0') {
        printf("    %s%s%s\n", COLOR_YELLOW, message, COLOR_RESET);
    }
    printf("    Expected: %lu\n", expected);
    printf("    Got:      %lu\n", actual);
    current_test = NULL;
}

void test_fail_str(const char *message, const char *expected, const char *actual)
{
    if (current_test == NULL) {
        return;
    }

    printf("  %s[FAIL]%s %s\n", COLOR_RED, COLOR_RESET, current_test);
    if (message != NULL && message[0] != '\0') {
        printf("    %s%s%s\n", COLOR_YELLOW, message, COLOR_RESET);
    }
    printf("    Expected: \"%s\"\n", expected);
    printf("    Got:      \"%s\"\n", actual);
    current_test = NULL;
}

void test_fail_bytes(const char *message, const uint8_t *expected,
                     const uint8_t *actual, size_t len)
{
    if (current_test == NULL) {
        return;
    }

    printf("  %s[FAIL]%s %s\n", COLOR_RED, COLOR_RESET, current_test);
    if (message != NULL && message[0] != '\0') {
        printf("    %s%s%s\n", COLOR_YELLOW, message, COLOR_RESET);
    }
    printf("    Expected: ");
    print_hex(expected, len);
    printf("\n");
    printf("    Got:      ");
    print_hex(actual, len);
    printf("\n");
    current_test = NULL;
}

void test_section(const char *section_name)
{
    printf("%s%s:%s\n", COLOR_CYAN, section_name, COLOR_RESET);
}

int test_global_summary(void)
{
    int all_passed = (global_tests_passed == global_tests_run);

    printf("%s", COLOR_BOLD);
    printf("================================================================================\n");
    printf("                            GLOBAL TEST SUMMARY\n");
    printf("================================================================================\n");
    printf("%s", COLOR_RESET);
    printf("\n");

    printf("  Test Suites: ");
    if (global_suites_passed == global_suites_run) {
        printf("%s%d/%d passed%s\n", COLOR_GREEN,
               global_suites_passed, global_suites_run, COLOR_RESET);
    } else {
        printf("%s%d/%d passed (%d failed)%s\n", COLOR_RED,
               global_suites_passed, global_suites_run,
               global_suites_run - global_suites_passed, COLOR_RESET);
    }

    printf("  Test Cases:  ");
    if (all_passed) {
        printf("%s%d/%d passed%s\n", COLOR_GREEN,
               global_tests_passed, global_tests_run, COLOR_RESET);
    } else {
        printf("%s%d/%d passed (%d failed)%s\n", COLOR_RED,
               global_tests_passed, global_tests_run,
               global_tests_run - global_tests_passed, COLOR_RESET);
    }

    printf("\n");
    printf("%s", COLOR_BOLD);
    if (all_passed) {
        printf("%s                         ALL TESTS PASSED!%s\n",
               COLOR_GREEN, COLOR_RESET);
    } else {
        printf("%s                         SOME TESTS FAILED%s\n",
               COLOR_RED, COLOR_RESET);
    }
    printf("%s", COLOR_BOLD);
    printf("================================================================================\n");
    printf("%s", COLOR_RESET);

    return all_passed ? 0 : 1;
}
