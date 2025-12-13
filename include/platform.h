/*
 * Bitcoin Echo — Platform Abstraction Interface
 *
 * This header defines the complete interface between Bitcoin Echo and the
 * underlying operating system. All platform-specific code is isolated behind
 * this boundary. The consensus engine never calls anything outside this API.
 *
 * Implementations:
 *   - src/platform/posix.c  (Linux, macOS, BSD)
 *   - src/platform/win32.c  (Windows)
 *
 * Design principles:
 *   - Minimal surface area: only what Bitcoin needs
 *   - No leaky abstractions: platform details stay in implementation
 *   - Blocking by default: simpler than async for our use case
 *   - Opaque types: hide platform-specific structures
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_PLATFORM_H
#define ECHO_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/*
 * ============================================================================
 * Opaque Types
 * ============================================================================
 *
 * These types are defined per-platform in the implementation files.
 * Callers interact only through pointers; the actual structure contents
 * vary by platform (e.g., int fd on POSIX, SOCKET on Windows).
 */

typedef struct plat_socket plat_socket_t;
typedef struct plat_thread plat_thread_t;
typedef struct plat_mutex plat_mutex_t;
typedef struct plat_cond plat_cond_t;

/*
 * ============================================================================
 * Error Codes
 * ============================================================================
 */

#define PLAT_OK 0             /* Operation succeeded */
#define PLAT_ERR (-1)         /* General error */
#define PLAT_ERR_TIMEOUT (-2) /* Operation timed out */
#define PLAT_ERR_CLOSED (-3)  /* Connection closed by peer */

/*
 * ============================================================================
 * Networking
 * ============================================================================
 *
 * TCP socket operations for Bitcoin P2P communication.
 * All operations are blocking. Non-blocking I/O is not supported.
 */

/*
 * Allocate a socket structure.
 *
 * Returns:
 *   Pointer to allocated socket, or NULL on failure
 *
 * Notes:
 *   - Must call plat_socket_free() when done
 */
plat_socket_t *plat_socket_alloc(void);

/*
 * Free a socket structure.
 *
 * Parameters:
 *   sock - Socket to free (may be NULL)
 *
 * Notes:
 *   - Socket should already be closed via plat_socket_close()
 */
void plat_socket_free(plat_socket_t *sock);

/*
 * Create a TCP socket.
 *
 * Parameters:
 *   sock  - Pointer to socket structure to initialize
 *
 * Returns:
 *   PLAT_OK on success, PLAT_ERR on failure
 *
 * Notes:
 *   - Socket is created but not connected
 *   - Must call plat_socket_close() when done, even on error paths
 */
int plat_socket_create(plat_socket_t *sock);

/*
 * Connect socket to remote host.
 *
 * Parameters:
 *   sock  - Previously created socket
 *   host  - Hostname or IP address (e.g., "192.168.1.1" or
 * "seed.bitcoin.sipa.be") port  - Port number (e.g., 8333 for mainnet)
 *
 * Returns:
 *   PLAT_OK on success, PLAT_ERR on failure
 *
 * Notes:
 *   - Blocks until connection established or fails
 *   - DNS resolution happens internally if hostname provided
 */
int plat_socket_connect(plat_socket_t *sock, const char *host, uint16_t port);

/*
 * Bind socket to port and begin listening for connections.
 *
 * Parameters:
 *   sock    - Previously created socket
 *   port    - Port number to listen on
 *   backlog - Maximum pending connections queue length
 *
 * Returns:
 *   PLAT_OK on success, PLAT_ERR on failure
 *
 * Notes:
 *   - Binds to all interfaces (0.0.0.0)
 *   - Socket becomes a listening socket, cannot be used for send/recv
 */
int plat_socket_listen(plat_socket_t *sock, uint16_t port, int backlog);

/*
 * Accept incoming connection on listening socket.
 *
 * Parameters:
 *   listener - Socket in listening state (from plat_socket_listen)
 *   client   - Pointer to socket structure for accepted connection
 *
 * Returns:
 *   PLAT_OK on success, PLAT_ERR on failure
 *
 * Notes:
 *   - Blocks until connection arrives
 *   - client socket is ready for send/recv on success
 */
int plat_socket_accept(plat_socket_t *listener, plat_socket_t *client);

/*
 * Send data on connected socket.
 *
 * Parameters:
 *   sock - Connected socket
 *   buf  - Data to send
 *   len  - Number of bytes to send
 *
 * Returns:
 *   Number of bytes sent (may be less than len), or negative on error:
 *     PLAT_ERR        - Send failed
 *     PLAT_ERR_CLOSED - Connection closed by peer
 *
 * Notes:
 *   - May send fewer bytes than requested; caller must retry with remainder
 *   - Blocks if send buffer is full
 */
int plat_socket_send(plat_socket_t *sock, const void *buf, size_t len);

/*
 * Receive data from connected socket.
 *
 * Parameters:
 *   sock - Connected socket
 *   buf  - Buffer to receive into
 *   len  - Maximum bytes to receive (buffer size)
 *
 * Returns:
 *   Number of bytes received, or:
 *     0               - Connection closed gracefully
 *     PLAT_ERR        - Receive failed
 *     PLAT_ERR_CLOSED - Connection reset by peer
 *
 * Notes:
 *   - Blocks until at least 1 byte available or connection closes
 *   - May receive fewer bytes than buffer size
 */
int plat_socket_recv(plat_socket_t *sock, void *buf, size_t len);

/*
 * Close socket and release resources.
 *
 * Parameters:
 *   sock - Socket to close
 *
 * Notes:
 *   - Safe to call on already-closed or never-connected sockets
 *   - Must be called even if create/connect failed
 */
void plat_socket_close(plat_socket_t *sock);

/*
 * Resolve hostname to IP address string.
 *
 * Parameters:
 *   host   - Hostname to resolve (e.g., "seed.bitcoin.sipa.be")
 *   ip_out - Buffer for resulting IP address string
 *   ip_len - Size of ip_out buffer (should be at least 46 for IPv6)
 *
 * Returns:
 *   PLAT_OK on success, PLAT_ERR on failure
 *
 * Notes:
 *   - Returns first resolved address (IPv4 preferred)
 *   - Result is null-terminated string (e.g., "192.168.1.1")
 */
int plat_dns_resolve(const char *host, char *ip_out, size_t ip_len);

/*
 * ============================================================================
 * Threading
 * ============================================================================
 *
 * Thread creation and synchronization primitives.
 * Modeled after POSIX threads for simplicity.
 */

/*
 * Create and start a new thread.
 *
 * Parameters:
 *   thread - Pointer to thread structure to initialize
 *   fn     - Thread entry point function
 *   arg    - Argument passed to fn
 *
 * Returns:
 *   PLAT_OK on success, PLAT_ERR on failure
 *
 * Notes:
 *   - Thread starts immediately upon successful return
 *   - fn receives arg and should return void* (ignored)
 */
int plat_thread_create(plat_thread_t *thread, void *(*fn)(void *), void *arg);

/*
 * Wait for thread to finish.
 *
 * Parameters:
 *   thread - Thread to wait for
 *
 * Returns:
 *   PLAT_OK on success, PLAT_ERR on failure
 *
 * Notes:
 *   - Blocks until thread exits
 *   - Thread resources are released after join
 */
int plat_thread_join(plat_thread_t *thread);

/*
 * Allocate a mutex structure.
 *
 * Returns:
 *   Pointer to allocated mutex, or NULL on failure
 *
 * Notes:
 *   - Must call plat_mutex_free() when done
 */
plat_mutex_t *plat_mutex_alloc(void);

/*
 * Free a mutex structure.
 *
 * Parameters:
 *   mutex - Mutex to free (may be NULL)
 *
 * Notes:
 *   - Mutex should already be destroyed via plat_mutex_destroy()
 */
void plat_mutex_free(plat_mutex_t *mutex);

/*
 * Initialize a mutex.
 *
 * Parameters:
 *   mutex - Pointer to mutex structure to initialize
 */
void plat_mutex_init(plat_mutex_t *mutex);

/*
 * Destroy a mutex.
 *
 * Parameters:
 *   mutex - Mutex to destroy
 *
 * Notes:
 *   - Must not be locked when destroyed
 */
void plat_mutex_destroy(plat_mutex_t *mutex);

/*
 * Lock a mutex.
 *
 * Parameters:
 *   mutex - Mutex to lock
 *
 * Notes:
 *   - Blocks until mutex acquired
 *   - Undefined behavior if same thread locks twice (no recursive mutexes)
 */
void plat_mutex_lock(plat_mutex_t *mutex);

/*
 * Unlock a mutex.
 *
 * Parameters:
 *   mutex - Mutex to unlock
 *
 * Notes:
 *   - Must be called by same thread that locked
 */
void plat_mutex_unlock(plat_mutex_t *mutex);

/*
 * Initialize a condition variable.
 *
 * Parameters:
 *   cond - Pointer to condition variable to initialize
 */
void plat_cond_init(plat_cond_t *cond);

/*
 * Destroy a condition variable.
 *
 * Parameters:
 *   cond - Condition variable to destroy
 *
 * Notes:
 *   - No threads should be waiting when destroyed
 */
void plat_cond_destroy(plat_cond_t *cond);

/*
 * Wait on condition variable.
 *
 * Parameters:
 *   cond  - Condition variable to wait on
 *   mutex - Mutex that must be held (will be released during wait)
 *
 * Notes:
 *   - Atomically releases mutex and waits
 *   - Re-acquires mutex before returning
 *   - May wake spuriously; always recheck condition in a loop
 */
void plat_cond_wait(plat_cond_t *cond, plat_mutex_t *mutex);

/*
 * Wait on condition variable with timeout.
 *
 * Parameters:
 *   cond  - Condition variable to wait on
 *   mutex - Mutex that must be held
 *   ms    - Timeout in milliseconds
 *
 * Returns:
 *   PLAT_OK if signaled, PLAT_ERR_TIMEOUT if timed out
 *
 * Notes:
 *   - Same semantics as plat_cond_wait, but with timeout
 */
int plat_cond_timedwait(plat_cond_t *cond, plat_mutex_t *mutex, uint32_t ms);

/*
 * Signal one thread waiting on condition variable.
 *
 * Parameters:
 *   cond - Condition variable to signal
 *
 * Notes:
 *   - Wakes at most one waiting thread
 *   - No effect if no threads waiting
 */
void plat_cond_signal(plat_cond_t *cond);

/*
 * Signal all threads waiting on condition variable.
 *
 * Parameters:
 *   cond - Condition variable to signal
 *
 * Notes:
 *   - Wakes all waiting threads
 */
void plat_cond_broadcast(plat_cond_t *cond);

/*
 * ============================================================================
 * File System
 * ============================================================================
 *
 * File operations for block storage and database.
 * Paths are UTF-8 encoded on all platforms.
 */

/*
 * Read entire file into dynamically allocated buffer.
 *
 * Parameters:
 *   path - Path to file
 *   data - Output: pointer to allocated buffer (caller must free)
 *   len  - Output: number of bytes read
 *
 * Returns:
 *   PLAT_OK on success, PLAT_ERR on failure
 *
 * Notes:
 *   - Allocates buffer with malloc(); caller must free()
 *   - On error, *data is NULL and *len is 0
 */
int plat_file_read(const char *path, uint8_t **data, size_t *len);

/*
 * Write buffer to file, replacing existing content.
 *
 * Parameters:
 *   path - Path to file
 *   data - Data to write
 *   len  - Number of bytes to write
 *
 * Returns:
 *   PLAT_OK on success, PLAT_ERR on failure
 *
 * Notes:
 *   - Creates file if it doesn't exist
 *   - Truncates existing file
 */
int plat_file_write(const char *path, const uint8_t *data, size_t len);

/*
 * Append buffer to file.
 *
 * Parameters:
 *   path - Path to file
 *   data - Data to append
 *   len  - Number of bytes to append
 *
 * Returns:
 *   PLAT_OK on success, PLAT_ERR on failure
 *
 * Notes:
 *   - Creates file if it doesn't exist
 */
int plat_file_append(const char *path, const uint8_t *data, size_t len);

/*
 * Atomically rename file.
 *
 * Parameters:
 *   old_path - Current file path
 *   new_path - New file path
 *
 * Returns:
 *   PLAT_OK on success, PLAT_ERR on failure
 *
 * Notes:
 *   - Atomic on POSIX (rename(2))
 *   - On Windows, may not be atomic if destination exists
 */
int plat_file_rename(const char *old_path, const char *new_path);

/*
 * Delete file.
 *
 * Parameters:
 *   path - Path to file to delete
 *
 * Returns:
 *   PLAT_OK on success, PLAT_ERR on failure
 */
int plat_file_delete(const char *path);

/*
 * Check if file exists.
 *
 * Parameters:
 *   path - Path to check
 *
 * Returns:
 *   1 if file exists, 0 if not
 */
int plat_file_exists(const char *path);

/*
 * Create directory, including parent directories.
 *
 * Parameters:
 *   path - Path to directory to create
 *
 * Returns:
 *   PLAT_OK on success (or already exists), PLAT_ERR on failure
 *
 * Notes:
 *   - Creates parent directories as needed (like mkdir -p)
 *   - Returns OK if directory already exists
 */
int plat_dir_create(const char *path);

/*
 * ============================================================================
 * Time
 * ============================================================================
 */

/*
 * Get current wall-clock time.
 *
 * Returns:
 *   Milliseconds since Unix epoch (1970-01-01 00:00:00 UTC)
 *
 * Notes:
 *   - May jump forward or backward (e.g., NTP adjustment)
 *   - Use for timestamps, not interval measurement
 */
uint64_t plat_time_ms(void);

/*
 * Get monotonic time for interval measurement.
 *
 * Returns:
 *   Milliseconds since arbitrary fixed point
 *
 * Notes:
 *   - Never goes backward
 *   - Use for measuring elapsed time, timeouts
 *   - Epoch is undefined; only differences are meaningful
 */
uint64_t plat_monotonic_ms(void);

/*
 * Sleep for specified duration.
 *
 * Parameters:
 *   ms - Milliseconds to sleep
 *
 * Notes:
 *   - May sleep slightly longer due to scheduling
 *   - Minimum resolution varies by platform
 */
void plat_sleep_ms(uint32_t ms);

/*
 * ============================================================================
 * Entropy
 * ============================================================================
 */

/*
 * Fill buffer with cryptographically secure random bytes.
 *
 * Parameters:
 *   buf - Buffer to fill
 *   len - Number of bytes requested
 *
 * Returns:
 *   PLAT_OK on success, PLAT_ERR on failure
 *
 * Notes:
 *   - Uses /dev/urandom on POSIX, BCryptGenRandom on Windows
 *   - Failure is rare but possible (e.g., early boot)
 */
int plat_random_bytes(uint8_t *buf, size_t len);

#endif /* ECHO_PLATFORM_H */
