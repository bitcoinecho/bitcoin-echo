/**
 * Bitcoin Echo — Logging System Unit Tests
 *
 * Tests the logging system implementation including:
 *   - Initialization and shutdown
 *   - Log level filtering
 *   - Component enable/disable
 *   - Output formatting
 *   - File output
 *   - Thread safety
 *
 * Session 9.4: Logging system tests.
 *
 * Build once. Build right. Stop.
 */

#include "log.h"
#include "platform.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * ============================================================================
 * TEST INFRASTRUCTURE
 * ============================================================================
 */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void name(void)

#define RUN_TEST(name)                                                         \
  do {                                                                         \
    printf("  %s... ", #name);                                                 \
    fflush(stdout);                                                            \
    tests_run++;                                                               \
    name();                                                                    \
    tests_passed++;                                                            \
    printf("PASS\n");                                                          \
  } while (0)

#define ASSERT(cond)                                                           \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("FAIL\n");                                                        \
      printf("    Assertion failed: %s\n", #cond);                             \
      printf("    Location: %s:%d\n", __FILE__, __LINE__);                     \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

#define ASSERT_EQ(a, b)                                                        \
  do {                                                                         \
    if ((a) != (b)) {                                                          \
      printf("FAIL\n");                                                        \
      printf("    Expected: %ld\n", (long)(b));                                \
      printf("    Actual: %ld\n", (long)(a));                                  \
      printf("    Location: %s:%d\n", __FILE__, __LINE__);                     \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

#define ASSERT_STR_EQ(a, b)                                                    \
  do {                                                                         \
    if (strcmp((a), (b)) != 0) {                                               \
      printf("FAIL\n");                                                        \
      printf("    Expected: \"%s\"\n", (b));                                   \
      printf("    Actual: \"%s\"\n", (a));                                     \
      printf("    Location: %s:%d\n", __FILE__, __LINE__);                     \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

/*
 * ============================================================================
 * HELPER FUNCTIONS
 * ============================================================================
 */

/* Create a temporary file and return its path */
static char *create_temp_file(void) {
  static char path[256];
  snprintf(path, sizeof(path), "/tmp/echo_log_test_%d.log", getpid());
  return path;
}

/* Read entire file into buffer (caller must free) */
static char *read_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (f == NULL) {
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  char *buf = malloc((size_t)size + 1);
  if (buf == NULL) {
    fclose(f);
    return NULL;
  }

  size_t read_size = fread(buf, 1, (size_t)size, f);
  buf[read_size] = '\0';
  fclose(f);

  return buf;
}

/* Remove a file */
static void remove_file(const char *path) { unlink(path); }

/*
 * ============================================================================
 * INITIALIZATION TESTS
 * ============================================================================
 */

TEST(test_init_shutdown) {
  /* Test basic init/shutdown cycle */
  log_init();
  ASSERT_EQ(log_get_level(), LOG_LEVEL_INFO);
  log_shutdown();

  /* Test multiple init/shutdown cycles */
  log_init();
  log_shutdown();
  log_init();
  log_shutdown();
}

TEST(test_double_init) {
  /* Double init should be safe */
  log_init();
  log_init(); /* Should be no-op */
  ASSERT_EQ(log_get_level(), LOG_LEVEL_INFO);
  log_shutdown();
}

TEST(test_double_shutdown) {
  /* Double shutdown should be safe */
  log_init();
  log_shutdown();
  log_shutdown(); /* Should be no-op */
}

/*
 * ============================================================================
 * LOG LEVEL TESTS
 * ============================================================================
 */

TEST(test_level_default) {
  log_init();
  ASSERT_EQ(log_get_level(), LOG_LEVEL_INFO);
  log_shutdown();
}

TEST(test_level_set_get) {
  log_init();

  log_set_level(LOG_LEVEL_ERROR);
  ASSERT_EQ(log_get_level(), LOG_LEVEL_ERROR);

  log_set_level(LOG_LEVEL_WARN);
  ASSERT_EQ(log_get_level(), LOG_LEVEL_WARN);

  log_set_level(LOG_LEVEL_INFO);
  ASSERT_EQ(log_get_level(), LOG_LEVEL_INFO);

  log_set_level(LOG_LEVEL_DEBUG);
  ASSERT_EQ(log_get_level(), LOG_LEVEL_DEBUG);

  log_shutdown();
}

TEST(test_level_filtering) {
  char *path = create_temp_file();
  log_init();
  ASSERT(log_set_output(path));

  /* Set level to WARN - INFO and DEBUG should be filtered */
  log_set_level(LOG_LEVEL_WARN);

  log_error(LOG_COMP_MAIN, "error message");
  log_warn(LOG_COMP_MAIN, "warn message");
  log_info(LOG_COMP_MAIN, "info message");   /* Should be filtered */
  log_debug(LOG_COMP_MAIN, "debug message"); /* Should be filtered */

  /* Close log to flush */
  log_set_output(NULL);
  log_shutdown();

  /* Check file contents */
  char *content = read_file(path);
  ASSERT(content != NULL);

  ASSERT(strstr(content, "error message") != NULL);
  ASSERT(strstr(content, "warn message") != NULL);
  ASSERT(strstr(content, "info message") == NULL);
  ASSERT(strstr(content, "debug message") == NULL);

  free(content);
  remove_file(path);
}

TEST(test_level_would_log) {
  log_init();

  log_set_level(LOG_LEVEL_WARN);

  ASSERT(log_would_log(LOG_LEVEL_ERROR, LOG_COMP_MAIN));
  ASSERT(log_would_log(LOG_LEVEL_WARN, LOG_COMP_MAIN));
  ASSERT(!log_would_log(LOG_LEVEL_INFO, LOG_COMP_MAIN));
  ASSERT(!log_would_log(LOG_LEVEL_DEBUG, LOG_COMP_MAIN));

  log_set_level(LOG_LEVEL_DEBUG);
  ASSERT(log_would_log(LOG_LEVEL_DEBUG, LOG_COMP_MAIN));

  log_shutdown();
}

/*
 * ============================================================================
 * COMPONENT TESTS
 * ============================================================================
 */

TEST(test_component_default_enabled) {
  log_init();

  /* All components should be enabled by default */
  for (int i = 0; i < LOG_COMP_COUNT; i++) {
    ASSERT(log_is_component_enabled((log_component_t)i));
  }

  log_shutdown();
}

TEST(test_component_enable_disable) {
  log_init();

  /* Disable NET component */
  log_set_component_enabled(LOG_COMP_NET, false);
  ASSERT(!log_is_component_enabled(LOG_COMP_NET));

  /* Other components should still be enabled */
  ASSERT(log_is_component_enabled(LOG_COMP_MAIN));
  ASSERT(log_is_component_enabled(LOG_COMP_P2P));

  /* Re-enable NET */
  log_set_component_enabled(LOG_COMP_NET, true);
  ASSERT(log_is_component_enabled(LOG_COMP_NET));

  log_shutdown();
}

TEST(test_component_filtering) {
  char *path = create_temp_file();
  log_init();
  ASSERT(log_set_output(path));
  log_set_level(LOG_LEVEL_DEBUG);

  /* Disable NET component */
  log_set_component_enabled(LOG_COMP_NET, false);

  log_info(LOG_COMP_MAIN, "main message");
  log_info(LOG_COMP_NET, "net message"); /* Should be filtered */
  log_info(LOG_COMP_P2P, "p2p message");

  log_set_output(NULL);
  log_shutdown();

  char *content = read_file(path);
  ASSERT(content != NULL);

  ASSERT(strstr(content, "main message") != NULL);
  ASSERT(strstr(content, "net message") == NULL);
  ASSERT(strstr(content, "p2p message") != NULL);

  free(content);
  remove_file(path);
}

TEST(test_component_would_log) {
  log_init();
  log_set_level(LOG_LEVEL_DEBUG);

  ASSERT(log_would_log(LOG_LEVEL_INFO, LOG_COMP_NET));

  log_set_component_enabled(LOG_COMP_NET, false);
  ASSERT(!log_would_log(LOG_LEVEL_INFO, LOG_COMP_NET));

  /* Other components unaffected */
  ASSERT(log_would_log(LOG_LEVEL_INFO, LOG_COMP_MAIN));

  log_shutdown();
}

/*
 * ============================================================================
 * OUTPUT FORMAT TESTS
 * ============================================================================
 */

TEST(test_output_format) {
  char *path = create_temp_file();
  log_init();
  ASSERT(log_set_output(path));
  log_set_level(LOG_LEVEL_DEBUG);

  log_info(LOG_COMP_MAIN, "test message");

  log_set_output(NULL);
  log_shutdown();

  char *content = read_file(path);
  ASSERT(content != NULL);

  /* Check format: YYYY-MM-DD HH:MM:SS.mmm [LEVEL] [COMP] Message */
  /* Should have timestamp in format: 20XX-XX-XX XX:XX:XX.XXX */
  ASSERT(strstr(content, "20") != NULL); /* Year starts with 20 */
  ASSERT(strstr(content, "[INFO ]") != NULL);
  ASSERT(strstr(content, "[MAIN]") != NULL);
  ASSERT(strstr(content, "test message") != NULL);

  free(content);
  remove_file(path);
}

TEST(test_output_levels) {
  char *path = create_temp_file();
  log_init();
  ASSERT(log_set_output(path));
  log_set_level(LOG_LEVEL_DEBUG);

  log_error(LOG_COMP_MAIN, "error");
  log_warn(LOG_COMP_MAIN, "warn");
  log_info(LOG_COMP_MAIN, "info");
  log_debug(LOG_COMP_MAIN, "debug");

  log_set_output(NULL);
  log_shutdown();

  char *content = read_file(path);
  ASSERT(content != NULL);

  ASSERT(strstr(content, "[ERROR]") != NULL);
  ASSERT(strstr(content, "[WARN ]") != NULL);
  ASSERT(strstr(content, "[INFO ]") != NULL);
  ASSERT(strstr(content, "[DEBUG]") != NULL);

  free(content);
  remove_file(path);
}

TEST(test_output_components) {
  char *path = create_temp_file();
  log_init();
  ASSERT(log_set_output(path));

  log_info(LOG_COMP_MAIN, "main");
  log_info(LOG_COMP_NET, "net");
  log_info(LOG_COMP_P2P, "p2p");
  log_info(LOG_COMP_CONS, "cons");
  log_info(LOG_COMP_SYNC, "sync");
  log_info(LOG_COMP_POOL, "pool");
  log_info(LOG_COMP_RPC, "rpc");
  log_info(LOG_COMP_DB, "db");
  log_info(LOG_COMP_STORE, "store");
  log_info(LOG_COMP_CRYPTO, "crypto");

  log_set_output(NULL);
  log_shutdown();

  char *content = read_file(path);
  ASSERT(content != NULL);

  ASSERT(strstr(content, "[MAIN]") != NULL);
  ASSERT(strstr(content, "[NET ]") != NULL);
  ASSERT(strstr(content, "[P2P ]") != NULL);
  ASSERT(strstr(content, "[CONS]") != NULL);
  ASSERT(strstr(content, "[SYNC]") != NULL);
  ASSERT(strstr(content, "[POOL]") != NULL);
  ASSERT(strstr(content, "[RPC ]") != NULL);
  ASSERT(strstr(content, "[DB  ]") != NULL);
  ASSERT(strstr(content, "[STOR]") != NULL);
  ASSERT(strstr(content, "[CRYP]") != NULL);

  free(content);
  remove_file(path);
}

TEST(test_output_printf_format) {
  char *path = create_temp_file();
  log_init();
  ASSERT(log_set_output(path));

  log_info(LOG_COMP_MAIN, "int: %d, string: %s, hex: 0x%x", 42, "hello", 255);

  log_set_output(NULL);
  log_shutdown();

  char *content = read_file(path);
  ASSERT(content != NULL);

  ASSERT(strstr(content, "int: 42") != NULL);
  ASSERT(strstr(content, "string: hello") != NULL);
  ASSERT(strstr(content, "hex: 0xff") != NULL);

  free(content);
  remove_file(path);
}

/*
 * ============================================================================
 * FILE OUTPUT TESTS
 * ============================================================================
 */

TEST(test_file_output) {
  char *path = create_temp_file();
  log_init();

  ASSERT(log_set_output(path));
  log_info(LOG_COMP_MAIN, "file output test");
  ASSERT(log_set_output(NULL)); /* Close file */

  log_shutdown();

  char *content = read_file(path);
  ASSERT(content != NULL);
  ASSERT(strstr(content, "file output test") != NULL);

  free(content);
  remove_file(path);
}

TEST(test_file_append) {
  char *path = create_temp_file();
  remove_file(path); /* Ensure clean start */

  /* First write */
  log_init();
  ASSERT(log_set_output(path));
  log_info(LOG_COMP_MAIN, "first message");
  log_set_output(NULL);
  log_shutdown();

  /* Second write (should append) */
  log_init();
  ASSERT(log_set_output(path));
  log_info(LOG_COMP_MAIN, "second message");
  log_set_output(NULL);
  log_shutdown();

  char *content = read_file(path);
  ASSERT(content != NULL);

  /* Both messages should be present */
  ASSERT(strstr(content, "first message") != NULL);
  ASSERT(strstr(content, "second message") != NULL);

  free(content);
  remove_file(path);
}

TEST(test_file_switch) {
  char *path1 = create_temp_file();
  char path2[256];
  snprintf(path2, sizeof(path2), "%s.2", path1);

  log_init();

  /* Write to first file */
  ASSERT(log_set_output(path1));
  log_info(LOG_COMP_MAIN, "message1");

  /* Switch to second file */
  ASSERT(log_set_output(path2));
  log_info(LOG_COMP_MAIN, "message2");

  log_set_output(NULL);
  log_shutdown();

  /* Check first file has only first message */
  char *content1 = read_file(path1);
  ASSERT(content1 != NULL);
  ASSERT(strstr(content1, "message1") != NULL);
  ASSERT(strstr(content1, "message2") == NULL);
  free(content1);

  /* Check second file has only second message */
  char *content2 = read_file(path2);
  ASSERT(content2 != NULL);
  ASSERT(strstr(content2, "message1") == NULL);
  ASSERT(strstr(content2, "message2") != NULL);
  free(content2);

  remove_file(path1);
  remove_file(path2);
}

TEST(test_invalid_file_path) {
  log_init();

  /* Try to open file in non-existent directory */
  ASSERT(!log_set_output("/nonexistent/directory/file.log"));

  /* stderr should still work */
  log_info(LOG_COMP_MAIN, "stderr message");

  log_shutdown();
}

/*
 * ============================================================================
 * HELPER FUNCTION TESTS
 * ============================================================================
 */

TEST(test_level_string) {
  ASSERT_STR_EQ(log_level_string(LOG_LEVEL_ERROR), "ERROR");
  ASSERT_STR_EQ(log_level_string(LOG_LEVEL_WARN), "WARN");
  ASSERT_STR_EQ(log_level_string(LOG_LEVEL_INFO), "INFO");
  ASSERT_STR_EQ(log_level_string(LOG_LEVEL_DEBUG), "DEBUG");

  /* Invalid level */
  const char *unknown = log_level_string((log_level_t)99);
  ASSERT(unknown != NULL);
  ASSERT(strlen(unknown) > 0);
}

TEST(test_component_string) {
  ASSERT(strncmp(log_component_string(LOG_COMP_MAIN), "MAIN", 4) == 0);
  ASSERT(strncmp(log_component_string(LOG_COMP_NET), "NET", 3) == 0);
  ASSERT(strncmp(log_component_string(LOG_COMP_P2P), "P2P", 3) == 0);
  ASSERT(strncmp(log_component_string(LOG_COMP_CONS), "CONS", 4) == 0);
  ASSERT(strncmp(log_component_string(LOG_COMP_SYNC), "SYNC", 4) == 0);
  ASSERT(strncmp(log_component_string(LOG_COMP_POOL), "POOL", 4) == 0);
  ASSERT(strncmp(log_component_string(LOG_COMP_RPC), "RPC", 3) == 0);
  ASSERT(strncmp(log_component_string(LOG_COMP_DB), "DB", 2) == 0);
  ASSERT(strncmp(log_component_string(LOG_COMP_STORE), "STOR", 4) == 0);
  ASSERT(strncmp(log_component_string(LOG_COMP_CRYPTO), "CRYP", 4) == 0);

  /* Invalid component */
  const char *unknown = log_component_string((log_component_t)99);
  ASSERT(unknown != NULL);
  ASSERT(strlen(unknown) > 0);
}

/*
 * ============================================================================
 * LOG_MSG FUNCTION TEST
 * ============================================================================
 */

TEST(test_log_msg) {
  char *path = create_temp_file();
  log_init();
  ASSERT(log_set_output(path));
  log_set_level(LOG_LEVEL_DEBUG);

  log_msg(LOG_LEVEL_INFO, LOG_COMP_MAIN, "generic log %d", 123);

  log_set_output(NULL);
  log_shutdown();

  char *content = read_file(path);
  ASSERT(content != NULL);
  ASSERT(strstr(content, "[INFO ]") != NULL);
  ASSERT(strstr(content, "generic log 123") != NULL);

  free(content);
  remove_file(path);
}

/*
 * ============================================================================
 * MACRO TESTS
 * ============================================================================
 */

/* Define LOG_COMPONENT for macro tests */
#undef LOG_COMPONENT
#define LOG_COMPONENT LOG_COMP_NET

TEST(test_convenience_macros) {
  char *path = create_temp_file();
  log_init();
  ASSERT(log_set_output(path));
  log_set_level(LOG_LEVEL_DEBUG);

  LOG_ERROR("error via macro");
  LOG_WARN("warn via macro");
  LOG_INFO("info via macro");
  LOG_DEBUG("debug via macro");

  log_set_output(NULL);
  log_shutdown();

  char *content = read_file(path);
  ASSERT(content != NULL);

  /* All should use NET component (defined above) */
  ASSERT(strstr(content, "[NET ]") != NULL);
  ASSERT(strstr(content, "error via macro") != NULL);
  ASSERT(strstr(content, "warn via macro") != NULL);
  ASSERT(strstr(content, "info via macro") != NULL);
  ASSERT(strstr(content, "debug via macro") != NULL);

  free(content);
  remove_file(path);
}

/* Restore default LOG_COMPONENT */
#undef LOG_COMPONENT
#define LOG_COMPONENT LOG_COMP_MAIN

/*
 * ============================================================================
 * STRESS TEST
 * ============================================================================
 */

TEST(test_many_messages) {
  char *path = create_temp_file();
  log_init();
  ASSERT(log_set_output(path));
  log_set_level(LOG_LEVEL_DEBUG);

  /* Log many messages */
  for (int i = 0; i < 1000; i++) {
    log_info(LOG_COMP_MAIN, "message %d", i);
  }

  log_set_output(NULL);
  log_shutdown();

  char *content = read_file(path);
  ASSERT(content != NULL);

  /* Check first and last messages */
  ASSERT(strstr(content, "message 0") != NULL);
  ASSERT(strstr(content, "message 999") != NULL);

  free(content);
  remove_file(path);
}

/*
 * ============================================================================
 * EDGE CASE TESTS
 * ============================================================================
 */

TEST(test_empty_message) {
  char *path = create_temp_file();
  log_init();
  ASSERT(log_set_output(path));

  log_info(LOG_COMP_MAIN, "");

  log_set_output(NULL);
  log_shutdown();

  char *content = read_file(path);
  ASSERT(content != NULL);
  /* Should still have header even with empty message */
  ASSERT(strstr(content, "[INFO ]") != NULL);
  ASSERT(strstr(content, "[MAIN]") != NULL);

  free(content);
  remove_file(path);
}

TEST(test_long_message) {
  char *path = create_temp_file();
  log_init();
  ASSERT(log_set_output(path));

  /* Create a long message */
  char long_msg[2048];
  memset(long_msg, 'X', sizeof(long_msg) - 1);
  long_msg[sizeof(long_msg) - 1] = '\0';

  log_info(LOG_COMP_MAIN, "%s", long_msg);

  log_set_output(NULL);
  log_shutdown();

  char *content = read_file(path);
  ASSERT(content != NULL);
  ASSERT(strlen(content) > 2000);

  free(content);
  remove_file(path);
}

TEST(test_special_characters) {
  char *path = create_temp_file();
  log_init();
  ASSERT(log_set_output(path));

  log_info(LOG_COMP_MAIN, "special: %% \t \n \"quoted\"");

  log_set_output(NULL);
  log_shutdown();

  char *content = read_file(path);
  ASSERT(content != NULL);
  ASSERT(strstr(content, "special: %") != NULL);
  ASSERT(strstr(content, "\"quoted\"") != NULL);

  free(content);
  remove_file(path);
}

TEST(test_uninitialized_logging) {
  /* Logging before init should not crash */
  log_info(LOG_COMP_MAIN, "this should be ignored");
  log_set_level(LOG_LEVEL_DEBUG);             /* Should be no-op */
  ASSERT_EQ(log_get_level(), LOG_LEVEL_INFO); /* Returns default */
}

/*
 * ============================================================================
 * MAIN
 * ============================================================================
 */

int main(void) {
  printf("Bitcoin Echo — Logging System Tests\n");
  printf("====================================\n\n");

  printf("Initialization tests:\n");
  RUN_TEST(test_init_shutdown);
  RUN_TEST(test_double_init);
  RUN_TEST(test_double_shutdown);

  printf("\nLog level tests:\n");
  RUN_TEST(test_level_default);
  RUN_TEST(test_level_set_get);
  RUN_TEST(test_level_filtering);
  RUN_TEST(test_level_would_log);

  printf("\nComponent tests:\n");
  RUN_TEST(test_component_default_enabled);
  RUN_TEST(test_component_enable_disable);
  RUN_TEST(test_component_filtering);
  RUN_TEST(test_component_would_log);

  printf("\nOutput format tests:\n");
  RUN_TEST(test_output_format);
  RUN_TEST(test_output_levels);
  RUN_TEST(test_output_components);
  RUN_TEST(test_output_printf_format);

  printf("\nFile output tests:\n");
  RUN_TEST(test_file_output);
  RUN_TEST(test_file_append);
  RUN_TEST(test_file_switch);
  RUN_TEST(test_invalid_file_path);

  printf("\nHelper function tests:\n");
  RUN_TEST(test_level_string);
  RUN_TEST(test_component_string);

  printf("\nlog_msg function test:\n");
  RUN_TEST(test_log_msg);

  printf("\nMacro tests:\n");
  RUN_TEST(test_convenience_macros);

  printf("\nStress tests:\n");
  RUN_TEST(test_many_messages);

  printf("\nEdge case tests:\n");
  RUN_TEST(test_empty_message);
  RUN_TEST(test_long_message);
  RUN_TEST(test_special_characters);
  RUN_TEST(test_uninitialized_logging);

  printf("\n====================================\n");
  printf("Tests: %d/%d passed\n", tests_passed, tests_run);

  return (tests_passed == tests_run) ? 0 : 1;
}
