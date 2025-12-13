/**
 * Bitcoin Echo — Minimal Logging System Implementation
 *
 * Implements a fixed-format, machine-parseable logging system.
 *
 * Design notes:
 *   - Uses a single mutex for thread safety
 *   - Formats directly to output to minimize buffering
 *   - Timestamp computation uses platform time functions
 *   - Component enable/disable uses bitfield for efficiency
 *
 * Session 9.4: Logging system implementation.
 *
 * Build once. Build right. Stop.
 */

#include "log.h"

#include "platform.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * ============================================================================
 * INTERNAL STATE
 * ============================================================================
 */

/* Log output file (NULL = stderr) */
static FILE *g_log_file = NULL;

/* Log file ownership (true if we opened it, false for stderr) */
static bool g_owns_file = false;

/* Current log level threshold */
static log_level_t g_log_level = LOG_LEVEL_INFO;

/* Component enable flags (bitfield) */
static uint32_t g_enabled_components = 0xFFFFFFFF; /* All enabled by default */

/* Thread safety mutex (pointer to dynamically allocated mutex) */
static plat_mutex_t *g_log_mutex = NULL;

/* Initialization flag */
static bool g_initialized = false;

/*
 * ============================================================================
 * STRING TABLES
 * ============================================================================
 */

/* Level names (fixed width for alignment) */
static const char *const LEVEL_STRINGS[] = {"ERROR", "WARN ", "INFO ", "DEBUG"};

/* Component names (4 chars max for fixed width) */
static const char *const COMPONENT_STRINGS[] = {
    "MAIN", /* LOG_COMP_MAIN */
    "NET ", /* LOG_COMP_NET */
    "P2P ", /* LOG_COMP_P2P */
    "CONS", /* LOG_COMP_CONS */
    "SYNC", /* LOG_COMP_SYNC */
    "POOL", /* LOG_COMP_POOL */
    "RPC ", /* LOG_COMP_RPC */
    "DB  ", /* LOG_COMP_DB */
    "STOR", /* LOG_COMP_STORE */
    "CRYP"  /* LOG_COMP_CRYPTO */
};

/*
 * ============================================================================
 * INTERNAL HELPERS
 * ============================================================================
 */

/**
 * Format timestamp into buffer.
 *
 * Format: YYYY-MM-DD HH:MM:SS.mmm
 *
 * Parameters:
 *   buf     - Output buffer (must be at least 24 bytes)
 *   buf_len - Buffer size
 *
 * Returns:
 *   Number of characters written (excluding null terminator)
 */
static size_t format_timestamp(char *buf, size_t buf_len) {
  if (buf_len < 24) {
    return 0;
  }

  /* Get current time */
  uint64_t now_ms = plat_time_ms();
  time_t now_sec = (time_t)(now_ms / 1000);
  uint32_t ms_part = (uint32_t)(now_ms % 1000);

  /* Convert to local time */
  struct tm tm_local;
  /* Use localtime_r for thread safety (POSIX) or localtime on Windows */
#ifdef _WIN32
  struct tm *tmp = localtime(&now_sec); /* Not thread-safe on Windows */
  if (tmp == NULL) {
    return 0;
  }
  tm_local = *tmp;
#else
  if (localtime_r(&now_sec, &tm_local) == NULL) {
    return 0;
  }
#endif

  /* Format: YYYY-MM-DD HH:MM:SS.mmm */
  int written =
      snprintf(buf, buf_len, "%04d-%02d-%02d %02d:%02d:%02d.%03u",
               tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
               tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec, ms_part);

  return (written > 0) ? (size_t)written : 0;
}

/**
 * Write a log message to the output.
 *
 * Format: YYYY-MM-DD HH:MM:SS.mmm [LEVEL] [COMP] Message
 *
 * Parameters:
 *   level  - Log level
 *   comp   - Component identifier
 *   format - printf-style format string
 *   args   - Format arguments
 */
static void log_write(log_level_t level, log_component_t comp,
                      const char *format, va_list args) {
  /* Quick check before acquiring lock */
  if (!g_initialized || g_log_mutex == NULL) {
    return;
  }

  /* Check level threshold */
  if (level > g_log_level) {
    return;
  }

  /* Check component enabled */
  if (comp < LOG_COMP_COUNT && !(g_enabled_components & (1U << comp))) {
    return;
  }

  /* Acquire lock */
  plat_mutex_lock(g_log_mutex);

  /* Get output file */
  FILE *out = (g_log_file != NULL) ? g_log_file : stderr;

  /* Format timestamp */
  char timestamp[32];
  format_timestamp(timestamp, sizeof(timestamp));

  /* Get level and component strings */
  const char *level_str =
      (level <= LOG_LEVEL_DEBUG) ? LEVEL_STRINGS[level] : "?????";
  const char *comp_str =
      (comp < LOG_COMP_COUNT) ? COMPONENT_STRINGS[comp] : "????";

  /* Write header: timestamp [level] [component] */
  fprintf(out, "%s [%s] [%s] ", timestamp, level_str, comp_str);

  /* Write message */
  vfprintf(out, format, args);

  /* Ensure newline */
  fprintf(out, "\n");

  /* Flush for immediate visibility (important for ERROR/WARN) */
  if (level <= LOG_LEVEL_WARN) {
    fflush(out);
  }

  plat_mutex_unlock(g_log_mutex);
}

/*
 * ============================================================================
 * PUBLIC FUNCTIONS - CONFIGURATION
 * ============================================================================
 */

void log_init(void) {
  if (g_initialized) {
    return;
  }

  /* Allocate mutex using platform API */
  g_log_mutex = plat_mutex_alloc();
  if (g_log_mutex == NULL) {
    return; /* Failed to allocate - can't initialize */
  }

  plat_mutex_init(g_log_mutex);
  g_log_file = NULL;
  g_owns_file = false;
  g_log_level = LOG_LEVEL_INFO;
  g_enabled_components = 0xFFFFFFFF; /* All enabled */
  g_initialized = true;
}

void log_shutdown(void) {
  if (!g_initialized || g_log_mutex == NULL) {
    return;
  }

  plat_mutex_lock(g_log_mutex);

  /* Close file if we own it */
  if (g_owns_file && g_log_file != NULL) {
    fclose(g_log_file);
  }
  g_log_file = NULL;
  g_owns_file = false;

  g_initialized = false;

  plat_mutex_unlock(g_log_mutex);
  plat_mutex_destroy(g_log_mutex);
  plat_mutex_free(g_log_mutex);
  g_log_mutex = NULL;
}

void log_set_level(log_level_t level) {
  if (!g_initialized || g_log_mutex == NULL) {
    return;
  }

  plat_mutex_lock(g_log_mutex);
  g_log_level = level;
  plat_mutex_unlock(g_log_mutex);
}

log_level_t log_get_level(void) {
  if (!g_initialized || g_log_mutex == NULL) {
    return LOG_LEVEL_INFO;
  }

  plat_mutex_lock(g_log_mutex);
  log_level_t level = g_log_level;
  plat_mutex_unlock(g_log_mutex);

  return level;
}

void log_set_component_enabled(log_component_t comp, bool enabled) {
  if (!g_initialized || g_log_mutex == NULL || comp >= LOG_COMP_COUNT) {
    return;
  }

  plat_mutex_lock(g_log_mutex);
  if (enabled) {
    g_enabled_components |= (1U << comp);
  } else {
    g_enabled_components &= ~(1U << comp);
  }
  plat_mutex_unlock(g_log_mutex);
}

bool log_is_component_enabled(log_component_t comp) {
  if (!g_initialized || g_log_mutex == NULL || comp >= LOG_COMP_COUNT) {
    return false;
  }

  plat_mutex_lock(g_log_mutex);
  bool enabled = (g_enabled_components & (1U << comp)) != 0;
  plat_mutex_unlock(g_log_mutex);

  return enabled;
}

bool log_set_output(const char *path) {
  if (!g_initialized || g_log_mutex == NULL) {
    return false;
  }

  plat_mutex_lock(g_log_mutex);

  /* Close previous file if we own it */
  if (g_owns_file && g_log_file != NULL) {
    fclose(g_log_file);
    g_log_file = NULL;
    g_owns_file = false;
  }

  if (path == NULL) {
    /* Revert to stderr */
    g_log_file = NULL;
    g_owns_file = false;
    plat_mutex_unlock(g_log_mutex);
    return true;
  }

  /* Open file in append mode */
  FILE *file = fopen(path, "a");
  if (file == NULL) {
    plat_mutex_unlock(g_log_mutex);
    return false;
  }

  g_log_file = file;
  g_owns_file = true;

  plat_mutex_unlock(g_log_mutex);
  return true;
}

/*
 * ============================================================================
 * PUBLIC FUNCTIONS - LOGGING
 * ============================================================================
 */

void log_error(log_component_t comp, const char *format, ...) {
  va_list args;
  va_start(args, format);
  log_write(LOG_LEVEL_ERROR, comp, format, args);
  va_end(args);
}

void log_warn(log_component_t comp, const char *format, ...) {
  va_list args;
  va_start(args, format);
  log_write(LOG_LEVEL_WARN, comp, format, args);
  va_end(args);
}

void log_info(log_component_t comp, const char *format, ...) {
  va_list args;
  va_start(args, format);
  log_write(LOG_LEVEL_INFO, comp, format, args);
  va_end(args);
}

void log_debug(log_component_t comp, const char *format, ...) {
  va_list args;
  va_start(args, format);
  log_write(LOG_LEVEL_DEBUG, comp, format, args);
  va_end(args);
}

void log_msg(log_level_t level, log_component_t comp, const char *format, ...) {
  va_list args;
  va_start(args, format);
  log_write(level, comp, format, args);
  va_end(args);
}

/*
 * ============================================================================
 * PUBLIC FUNCTIONS - HELPERS
 * ============================================================================
 */

const char *log_level_string(log_level_t level) {
  switch (level) {
  case LOG_LEVEL_ERROR:
    return "ERROR";
  case LOG_LEVEL_WARN:
    return "WARN";
  case LOG_LEVEL_INFO:
    return "INFO";
  case LOG_LEVEL_DEBUG:
    return "DEBUG";
  default:
    return "?????";
  }
}

const char *log_component_string(log_component_t comp) {
  if (comp >= LOG_COMP_COUNT) {
    return "????";
  }
  return COMPONENT_STRINGS[comp];
}

bool log_would_log(log_level_t level, log_component_t comp) {
  if (!g_initialized) {
    return false;
  }

  /* Quick check without lock for common case */
  if (level > g_log_level) {
    return false;
  }

  if (comp < LOG_COMP_COUNT && !(g_enabled_components & (1U << comp))) {
    return false;
  }

  return true;
}
