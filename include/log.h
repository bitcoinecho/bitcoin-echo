/**
 * Bitcoin Echo — Minimal Logging System
 *
 * Implements a fixed-format, machine-parseable logging system for the
 * Bitcoin Echo node. Design priorities:
 *
 *   - Machine-parseable: Fixed format enables automated analysis
 *   - Minimal overhead: No dynamic allocation, minimal formatting cost
 *   - Component-based: Log messages tagged by subsystem
 *   - Level-filtered: Runtime log level control
 *
 * Log format:
 *   YYYY-MM-DD HH:MM:SS.mmm [LEVEL] [COMPONENT] Message
 *
 * Example:
 *   2025-12-12 14:30:45.123 [INFO] [NET] Connected to 192.168.1.1:8333
 *   2025-12-12 14:30:45.456 [WARN] [CONS] Block 000000... has unusual timestamp
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_LOG_H
#define ECHO_LOG_H

#include <stdbool.h>

/*
 * ============================================================================
 * LOG LEVELS
 * ============================================================================
 *
 * Log levels in order of severity. Lower values are more severe.
 * A configured level filters out all messages with lower severity.
 */

typedef enum {
  LOG_LEVEL_ERROR = 0, /* Unrecoverable errors requiring attention */
  LOG_LEVEL_WARN = 1,  /* Recoverable problems, potential issues */
  LOG_LEVEL_INFO = 2,  /* Normal operational messages */
  LOG_LEVEL_DEBUG = 3  /* Detailed debugging information */
} log_level_t;

/*
 * ============================================================================
 * LOG COMPONENTS
 * ============================================================================
 *
 * Component identifiers for log message categorization.
 * Each component represents a major subsystem of the node.
 */

typedef enum {
  LOG_COMP_MAIN = 0, /* Main/general */
  LOG_COMP_NET,      /* Networking (peer connections, sockets) */
  LOG_COMP_P2P,      /* P2P protocol (messages, handshake) */
  LOG_COMP_CONS,     /* Consensus engine (validation, chain) */
  LOG_COMP_SYNC,     /* Block synchronization (IBD, headers) */
  LOG_COMP_POOL,     /* Mempool (transaction acceptance) */
  LOG_COMP_RPC,      /* RPC interface */
  LOG_COMP_DB,       /* Database operations */
  LOG_COMP_STORE,    /* Block storage */
  LOG_COMP_CRYPTO,   /* Cryptographic operations */
  LOG_COMP_COUNT     /* Number of components (not a valid component) */
} log_component_t;

/*
 * ============================================================================
 * LOG CONFIGURATION
 * ============================================================================
 */

/**
 * Initialize the logging system.
 *
 * Must be called before any logging functions. Sets default log level
 * to INFO and enables output to stderr.
 *
 * Thread-safe: Yes (uses internal mutex after init).
 */
void log_init(void);

/**
 * Shutdown the logging system.
 *
 * Flushes any buffered output and releases resources.
 * No logging calls should be made after this.
 */
void log_shutdown(void);

/**
 * Set the global log level.
 *
 * Messages with severity below this level will be discarded.
 *
 * Parameters:
 *   level - New log level threshold
 *
 * Thread-safe: Yes.
 */
void log_set_level(log_level_t level);

/**
 * Get the current log level.
 *
 * Returns:
 *   Current log level threshold
 *
 * Thread-safe: Yes.
 */
log_level_t log_get_level(void);

/**
 * Enable or disable a specific component.
 *
 * Disabled components produce no output regardless of level.
 * All components are enabled by default.
 *
 * Parameters:
 *   comp    - Component to configure
 *   enabled - true to enable, false to disable
 *
 * Thread-safe: Yes.
 */
void log_set_component_enabled(log_component_t comp, bool enabled);

/**
 * Check if a component is enabled.
 *
 * Parameters:
 *   comp - Component to check
 *
 * Returns:
 *   true if component is enabled
 *
 * Thread-safe: Yes.
 */
bool log_is_component_enabled(log_component_t comp);

/**
 * Set log output file.
 *
 * By default, logs go to stderr. This function allows redirecting
 * to a file. Pass NULL to revert to stderr.
 *
 * Parameters:
 *   path - Path to log file, or NULL for stderr
 *
 * Returns:
 *   true on success, false if file cannot be opened
 *
 * Notes:
 *   - File is opened in append mode
 *   - Previous file is closed when switching
 *   - Not thread-safe with concurrent log calls; call during init only
 */
bool log_set_output(const char *path);

/*
 * ============================================================================
 * LOGGING FUNCTIONS
 * ============================================================================
 *
 * All logging functions are thread-safe and non-blocking (to the extent
 * possible given file I/O constraints).
 */

/**
 * Log a message at ERROR level.
 *
 * Use for unrecoverable errors that require immediate attention.
 * Examples: database corruption, critical assertion failures.
 *
 * Parameters:
 *   comp   - Component identifier
 *   format - printf-style format string
 *   ...    - Format arguments
 */
void log_error(log_component_t comp, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * Log a message at WARN level.
 *
 * Use for recoverable problems or potential issues.
 * Examples: peer misbehavior, unusual block timestamps.
 *
 * Parameters:
 *   comp   - Component identifier
 *   format - printf-style format string
 *   ...    - Format arguments
 */
void log_warn(log_component_t comp, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * Log a message at INFO level.
 *
 * Use for normal operational events.
 * Examples: peer connected, block received, sync progress.
 *
 * Parameters:
 *   comp   - Component identifier
 *   format - printf-style format string
 *   ...    - Format arguments
 */
void log_info(log_component_t comp, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * Log a message at DEBUG level.
 *
 * Use for detailed debugging information.
 * Examples: individual message parsing, state transitions.
 *
 * Parameters:
 *   comp   - Component identifier
 *   format - printf-style format string
 *   ...    - Format arguments
 */
void log_debug(log_component_t comp, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * Generic log function with explicit level.
 *
 * Use when the level is determined at runtime.
 *
 * Parameters:
 *   level  - Log level
 *   comp   - Component identifier
 *   format - printf-style format string
 *   ...    - Format arguments
 */
void log_msg(log_level_t level, log_component_t comp, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

/*
 * ============================================================================
 * HELPER FUNCTIONS
 * ============================================================================
 */

/**
 * Get string name for a log level.
 *
 * Parameters:
 *   level - Log level
 *
 * Returns:
 *   Static string: "ERROR", "WARN", "INFO", or "DEBUG"
 */
const char *log_level_string(log_level_t level);

/**
 * Get short string name for a component.
 *
 * Parameters:
 *   comp - Component identifier
 *
 * Returns:
 *   Static string (4 chars max): "MAIN", "NET", "P2P", etc.
 */
const char *log_component_string(log_component_t comp);

/**
 * Check if a message at the given level would be logged.
 *
 * Useful to avoid expensive formatting when message won't be logged.
 *
 * Parameters:
 *   level - Log level to check
 *   comp  - Component to check
 *
 * Returns:
 *   true if a message would be logged
 */
bool log_would_log(log_level_t level, log_component_t comp);

/*
 * ============================================================================
 * CONVENIENCE MACROS
 * ============================================================================
 *
 * These macros provide the component implicitly for common use cases.
 * Using LOG_COMPONENT before including this header customizes them.
 */

#ifndef LOG_COMPONENT
#define LOG_COMPONENT LOG_COMP_MAIN
#endif

#define LOG_ERROR(...) log_error(LOG_COMPONENT, __VA_ARGS__)
#define LOG_WARN(...) log_warn(LOG_COMPONENT, __VA_ARGS__)
#define LOG_INFO(...) log_info(LOG_COMPONENT, __VA_ARGS__)
#define LOG_DEBUG(...) log_debug(LOG_COMPONENT, __VA_ARGS__)

/*
 * Conditional debug logging (can be compiled out for release builds).
 * Define ECHO_NO_DEBUG_LOGS to disable.
 */
#ifdef ECHO_NO_DEBUG_LOGS
#define LOG_DEBUG_IF(cond, ...) ((void)0)
#else
#define LOG_DEBUG_IF(cond, ...)                                                \
  do {                                                                         \
    if (cond)                                                                  \
      log_debug(LOG_COMPONENT, __VA_ARGS__);                                   \
  } while (0)
#endif

#endif /* ECHO_LOG_H */
