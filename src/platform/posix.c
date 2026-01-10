/*
 * Bitcoin Echo — POSIX Platform Implementation
 *
 * This file implements the platform abstraction interface for POSIX systems:
 * Linux, macOS, FreeBSD, and other Unix-like operating systems.
 *
 * Build once. Build right. Stop.
 */

/* Feature test macros must come before any includes */
#define _POSIX_C_SOURCE 200809L

/* Standard C headers */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* POSIX types and threading */
#include <pthread.h>
#include <sys/types.h>
#include <time.h>

/* Networking */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

/* File System */
#include <sys/stat.h>

/* Time */
#include <sys/time.h>

#include "platform.h"

/* Entropy - prefer getentropy on modern systems, fallback to /dev/urandom */
#if defined(__APPLE__) && defined(__MACH__)
#include <sys/random.h> /* getentropy on macOS 10.12+ */
#define HAVE_GETENTROPY 1
#elif defined(__linux__)
#include <sys/random.h> /* getrandom on Linux 3.17+ */
#define HAVE_GETRANDOM 1
#endif

/*
 * ============================================================================
 * Opaque Type Definitions
 * ============================================================================
 *
 * These define the actual contents of the opaque types declared in platform.h.
 * Only this file knows their structure; callers see only pointers.
 */

/*
 * Socket structure.
 * Wraps a file descriptor with state tracking.
 */
struct plat_socket {
  int fd; /* File descriptor, -1 if not open */
};

/*
 * Thread structure.
 * Wraps pthread_t handle.
 */
struct plat_thread {
  pthread_t handle;
};

/*
 * Mutex structure.
 * Wraps pthread_mutex_t.
 */
struct plat_mutex {
  pthread_mutex_t handle;
};

/*
 * Condition variable structure.
 * Wraps pthread_cond_t.
 */
struct plat_cond {
  pthread_cond_t handle;
};

/*
 * ============================================================================
 * Networking Implementation
 * ============================================================================
 */

plat_socket_t *plat_socket_alloc(void) {
  plat_socket_t *sock = malloc(sizeof(plat_socket_t));
  if (sock) {
    sock->fd = -1;
  }
  return sock;
}

void plat_socket_free(plat_socket_t *sock) { free(sock); }

int plat_socket_create(plat_socket_t *sock) {
  int fd;

  if (sock == NULL) {
    return PLAT_ERR;
  }

  /* Create TCP socket */
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    sock->fd = -1;
    return PLAT_ERR;
  }

  sock->fd = fd;
  return PLAT_OK;
}

int plat_socket_connect(plat_socket_t *sock, const char *host, uint16_t port) {
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  char port_str[6];
  int ret;

  if (sock == NULL || sock->fd < 0 || host == NULL) {
    return PLAT_ERR;
  }

  /* Convert port to string for getaddrinfo */
  snprintf(port_str, sizeof(port_str), "%u", port);

  /* Set up hints for getaddrinfo */
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;       /* IPv4 for now */
  hints.ai_socktype = SOCK_STREAM; /* TCP */
  hints.ai_protocol = IPPROTO_TCP;

  /* Resolve hostname */
  ret = getaddrinfo(host, port_str, &hints, &result);
  if (ret != 0) {
    return PLAT_ERR;
  }

  /* Set socket to non-blocking for connect with timeout */
  int flags = fcntl(sock->fd, F_GETFL, 0);
  if (flags < 0) {
    freeaddrinfo(result);
    return PLAT_ERR;
  }
  if (fcntl(sock->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    freeaddrinfo(result);
    return PLAT_ERR;
  }

  /* Try each address until one succeeds */
  int connected = 0;
  for (rp = result; rp != NULL && !connected; rp = rp->ai_next) {
    ret = connect(sock->fd, rp->ai_addr, rp->ai_addrlen);
    if (ret == 0) {
      /* Immediate connection (rare, but possible on localhost) */
      connected = 1;
    } else if (errno == EINPROGRESS) {
      /* Connection in progress - wait with timeout */
      fd_set write_fds;
      struct timeval tv;

      FD_ZERO(&write_fds);
      FD_SET(sock->fd, &write_fds);

      /* 500ms timeout - very fast fail for unreachable nodes.
       * During IBD we need to try many addresses quickly. Even 2 seconds
       * is too long when iterating through 40+ addresses per tick.
       * 500ms is aggressive but nodes that don't respond quickly are likely
       * unreachable or overloaded anyway. */
      tv.tv_sec = 0;
      tv.tv_usec = 500000; /* 500ms */

      ret = select(sock->fd + 1, NULL, &write_fds, NULL, &tv);
      if (ret > 0) {
        /* Check if connection succeeded */
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 &&
            error == 0) {
          connected = 1;
        }
      }
      /* ret == 0 means timeout, ret < 0 means error - try next address */
    }
    /* Other errors - try next address */
  }

  freeaddrinfo(result);

  /* Restore blocking mode for subsequent I/O */
  fcntl(sock->fd, F_SETFL, flags);

  return connected ? PLAT_OK : PLAT_ERR;
}

int plat_socket_listen(plat_socket_t *sock, uint16_t port, int backlog) {
  struct sockaddr_in addr;
  int optval = 1;

  if (sock == NULL || sock->fd < 0) {
    return PLAT_ERR;
  }

  /* Allow address reuse to avoid "address already in use" on restart */
  if (setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) <
      0) {
    return PLAT_ERR;
  }

  /* Bind to all interfaces on specified port */
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (bind(sock->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    return PLAT_ERR;
  }

  /* Start listening */
  if (listen(sock->fd, backlog) < 0) {
    return PLAT_ERR;
  }

  return PLAT_OK;
}

int plat_socket_accept(plat_socket_t *listener, plat_socket_t *client) {
  int fd;
  struct sockaddr_in addr;
  socklen_t addr_len = sizeof(addr);

  if (listener == NULL || listener->fd < 0 || client == NULL) {
    return PLAT_ERR;
  }

  /* Accept connection (blocks until one arrives) */
  fd = accept(listener->fd, (struct sockaddr *)&addr, &addr_len);
  if (fd < 0) {
    client->fd = -1;
    return PLAT_ERR;
  }

  client->fd = fd;
  return PLAT_OK;
}

int plat_socket_set_nonblocking(plat_socket_t *sock) {
  int flags;

  if (sock == NULL || sock->fd < 0) {
    return PLAT_ERR;
  }

  /* Get current flags */
  flags = fcntl(sock->fd, F_GETFL, 0);
  if (flags == -1) {
    return PLAT_ERR;
  }

  /* Set non-blocking flag */
  if (fcntl(sock->fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    return PLAT_ERR;
  }

  return PLAT_OK;
}

int plat_socket_connect_async(plat_socket_t *sock, const char *host,
                               uint16_t port) {
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  char port_str[6];
  int ret;

  if (sock == NULL || sock->fd < 0 || host == NULL) {
    return PLAT_ERR;
  }

  /* Convert port to string for getaddrinfo */
  snprintf(port_str, sizeof(port_str), "%u", port);

  /* Set up hints for getaddrinfo */
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;       /* IPv4 for now */
  hints.ai_socktype = SOCK_STREAM; /* TCP */
  hints.ai_protocol = IPPROTO_TCP;

  /* Resolve hostname */
  ret = getaddrinfo(host, port_str, &hints, &result);
  if (ret != 0) {
    return PLAT_ERR;
  }

  /* Set socket to non-blocking BEFORE connect */
  int flags = fcntl(sock->fd, F_GETFL, 0);
  if (flags < 0 || fcntl(sock->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    freeaddrinfo(result);
    return PLAT_ERR;
  }

  /* Start non-blocking connect - returns immediately with EINPROGRESS */
  int connected = 0;
  for (rp = result; rp != NULL && !connected; rp = rp->ai_next) {
    ret = connect(sock->fd, rp->ai_addr, rp->ai_addrlen);
    if (ret == 0) {
      /* Immediate connection (rare, localhost) - log this unusual case */
      fprintf(stderr, "[DEBUG] connect() returned 0 immediately for %s:%u\n",
              host, port);
      connected = 1;
    } else if (errno == EINPROGRESS) {
      /* Connection in progress - this is the expected case */
      freeaddrinfo(result);
      return PLAT_PENDING; /* Special return for "in progress" */
    }
  }

  freeaddrinfo(result);
  return connected ? PLAT_OK : PLAT_ERR;
}

int plat_socket_check_connect(plat_socket_t *sock) {
  if (sock == NULL || sock->fd < 0) {
    return PLAT_ERR;
  }

  /* Use select with 0 timeout to check if socket is writable (connected) */
  fd_set write_fds;
  struct timeval tv;

  FD_ZERO(&write_fds);
  FD_SET(sock->fd, &write_fds);

  tv.tv_sec = 0;
  tv.tv_usec = 0; /* Non-blocking check */

  int ret = select(sock->fd + 1, NULL, &write_fds, NULL, &tv);
  if (ret < 0) {
    return PLAT_ERR; /* select error */
  }
  if (ret == 0) {
    return PLAT_PENDING; /* Still connecting */
  }

  /* Socket is writable - check if connection succeeded or failed */
  int error = 0;
  socklen_t len = sizeof(error);
  if (getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
    return PLAT_ERR;
  }

  return (error == 0) ? PLAT_OK : PLAT_ERR;
}

int plat_socket_set_recv_timeout(plat_socket_t *sock, uint32_t timeout_ms) {
  if (sock == NULL || sock->fd < 0) {
    return PLAT_ERR;
  }

  struct timeval tv;
  tv.tv_sec = (time_t)(timeout_ms / 1000);
  tv.tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000);

  if (setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    return PLAT_ERR;
  }

  return PLAT_OK;
}

int plat_socket_send(plat_socket_t *sock, const void *buf, size_t len) {
  ssize_t sent;

  if (sock == NULL || sock->fd < 0 || buf == NULL) {
    return PLAT_ERR;
  }

  sent = send(sock->fd, buf, len, 0);
  if (sent < 0) {
    if (errno == EPIPE || errno == ECONNRESET) {
      return PLAT_ERR_CLOSED;
    }
    return PLAT_ERR;
  }

  return (int)sent;
}

int plat_socket_recv(plat_socket_t *sock, void *buf, size_t len) {
  ssize_t received;

  if (sock == NULL || sock->fd < 0 || buf == NULL) {
    return PLAT_ERR;
  }

  received = recv(sock->fd, buf, len, 0);
  if (received < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return PLAT_ERR_WOULD_BLOCK;
    }
    if (errno == ECONNRESET) {
      return PLAT_ERR_CLOSED;
    }
    return PLAT_ERR;
  }

  /* recv returns 0 on graceful close */
  return (int)received;
}

void plat_socket_close(plat_socket_t *sock) {
  if (sock == NULL) {
    return;
  }

  if (sock->fd >= 0) {
    close(sock->fd);
    sock->fd = -1;
  }
}

int plat_dns_resolve(const char *host, char *ip_out, size_t ip_len) {
  struct addrinfo hints;
  struct addrinfo *result;
  int ret;

  if (host == NULL || ip_out == NULL || ip_len == 0) {
    return PLAT_ERR;
  }

  /* Set up hints for getaddrinfo */
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET; /* IPv4 */
  hints.ai_socktype = SOCK_STREAM;

  /* Resolve hostname */
  ret = getaddrinfo(host, NULL, &hints, &result);
  if (ret != 0) {
    return PLAT_ERR;
  }

  if (result == NULL) {
    return PLAT_ERR;
  }

  /* Convert first result to string */
  /* Copy to properly aligned structure to avoid alignment issues */
  struct sockaddr_in addr_in_aligned;
  memcpy(&addr_in_aligned, result->ai_addr, sizeof(struct sockaddr_in));
  if (inet_ntop(AF_INET, &addr_in_aligned.sin_addr, ip_out,
                (socklen_t)ip_len) == NULL) {
    freeaddrinfo(result);
    return PLAT_ERR;
  }

  freeaddrinfo(result);
  return PLAT_OK;
}

int plat_dns_resolve_all(const char *host, char **ips_out, size_t ip_len,
                         size_t max_ips, size_t *count_out) {
  struct addrinfo hints;
  struct addrinfo *result;
  struct addrinfo *current;
  int ret;
  size_t count = 0;

  if (host == NULL || ips_out == NULL || count_out == NULL || max_ips == 0) {
    return PLAT_ERR;
  }

  /* Set up hints for getaddrinfo */
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET; /* IPv4 */
  hints.ai_socktype = SOCK_STREAM;

  /* Resolve hostname */
  ret = getaddrinfo(host, NULL, &hints, &result);
  if (ret != 0) {
    *count_out = 0;
    return PLAT_ERR;
  }

  if (result == NULL) {
    *count_out = 0;
    return PLAT_ERR;
  }

  /* Iterate through all results and convert to strings */
  for (current = result; current != NULL && count < max_ips;
       current = current->ai_next) {
    if (current->ai_family == AF_INET) {
      /* Copy to properly aligned structure to avoid alignment issues */
      struct sockaddr_in addr_in_aligned;
      memcpy(&addr_in_aligned, current->ai_addr, sizeof(struct sockaddr_in));
      if (inet_ntop(AF_INET, &addr_in_aligned.sin_addr, ips_out[count],
                    (socklen_t)ip_len) != NULL) {
        count++;
      }
    }
  }

  freeaddrinfo(result);
  *count_out = count;
  return (count > 0) ? PLAT_OK : PLAT_ERR;
}

/*
 * ============================================================================
 * Threading Implementation
 * ============================================================================
 */

int plat_thread_create(plat_thread_t *thread, void *(*fn)(void *), void *arg) {
  int ret;

  if (thread == NULL || fn == NULL) {
    return PLAT_ERR;
  }

  ret = pthread_create(&thread->handle, NULL, fn, arg);
  if (ret != 0) {
    return PLAT_ERR;
  }

  return PLAT_OK;
}

int plat_thread_join(plat_thread_t *thread) {
  int ret;

  if (thread == NULL) {
    return PLAT_ERR;
  }

  ret = pthread_join(thread->handle, NULL);
  if (ret != 0) {
    return PLAT_ERR;
  }

  return PLAT_OK;
}

plat_thread_t *plat_thread_alloc(void) {
  plat_thread_t *thread = malloc(sizeof(plat_thread_t));
  return thread;
}

void plat_thread_free(plat_thread_t *thread) { free(thread); }

plat_mutex_t *plat_mutex_alloc(void) {
  plat_mutex_t *mutex = malloc(sizeof(plat_mutex_t));
  return mutex;
}

void plat_mutex_free(plat_mutex_t *mutex) {
  if (mutex == NULL) {
    return;
  }
  free(mutex);
}

void plat_mutex_init(plat_mutex_t *mutex) {
  if (mutex == NULL) {
    return;
  }

  pthread_mutex_init(&mutex->handle, NULL);
}

void plat_mutex_destroy(plat_mutex_t *mutex) {
  if (mutex == NULL) {
    return;
  }

  pthread_mutex_destroy(&mutex->handle);
}

void plat_mutex_lock(plat_mutex_t *mutex) {
  if (mutex == NULL) {
    return;
  }

  pthread_mutex_lock(&mutex->handle);
}

void plat_mutex_unlock(plat_mutex_t *mutex) {
  if (mutex == NULL) {
    return;
  }

  pthread_mutex_unlock(&mutex->handle);
}

void plat_cond_init(plat_cond_t *cond) {
  if (cond == NULL) {
    return;
  }

  pthread_cond_init(&cond->handle, NULL);
}

void plat_cond_destroy(plat_cond_t *cond) {
  if (cond == NULL) {
    return;
  }

  pthread_cond_destroy(&cond->handle);
}

void plat_cond_wait(plat_cond_t *cond, plat_mutex_t *mutex) {
  if (cond == NULL || mutex == NULL) {
    return;
  }

  pthread_cond_wait(&cond->handle, &mutex->handle);
}

int plat_cond_timedwait(plat_cond_t *cond, plat_mutex_t *mutex, uint32_t ms) {
  struct timespec ts;
  int ret;

  if (cond == NULL || mutex == NULL) {
    return PLAT_ERR;
  }

  /* Get current time and add timeout */
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += (long)((ms % 1000) * 1000000);

  /* Handle nanosecond overflow */
  if (ts.tv_nsec >= 1000000000) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1000000000;
  }

  ret = pthread_cond_timedwait(&cond->handle, &mutex->handle, &ts);
  if (ret == ETIMEDOUT) {
    return PLAT_ERR_TIMEOUT;
  }
  if (ret != 0) {
    return PLAT_ERR;
  }

  return PLAT_OK;
}

void plat_cond_signal(plat_cond_t *cond) {
  if (cond == NULL) {
    return;
  }

  pthread_cond_signal(&cond->handle);
}

void plat_cond_broadcast(plat_cond_t *cond) {
  if (cond == NULL) {
    return;
  }

  pthread_cond_broadcast(&cond->handle);
}

/*
 * ============================================================================
 * File System Implementation
 * ============================================================================
 */

int plat_file_read(const char *path, uint8_t **data, size_t *len) {
  FILE *fp;
  long size;
  uint8_t *buf;
  size_t read_len;

  if (path == NULL || data == NULL || len == NULL) {
    return PLAT_ERR;
  }

  *data = NULL;
  *len = 0;

  fp = fopen(path, "rb");
  if (fp == NULL) {
    return PLAT_ERR;
  }

  /* Get file size */
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return PLAT_ERR;
  }

  size = ftell(fp);
  if (size < 0) {
    fclose(fp);
    return PLAT_ERR;
  }

  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return PLAT_ERR;
  }

  /* Allocate buffer */
  buf = malloc((size_t)size);
  if (buf == NULL) {
    fclose(fp);
    return PLAT_ERR;
  }

  /* Read entire file */
  read_len = fread(buf, 1, (size_t)size, fp);
  fclose(fp);

  if (read_len != (size_t)size) {
    free(buf);
    return PLAT_ERR;
  }

  *data = buf;
  *len = (size_t)size;
  return PLAT_OK;
}

int plat_file_write(const char *path, const uint8_t *data, size_t len) {
  FILE *fp;
  size_t written;

  if (path == NULL || (data == NULL && len > 0)) {
    return PLAT_ERR;
  }

  fp = fopen(path, "wb");
  if (fp == NULL) {
    return PLAT_ERR;
  }

  if (len > 0) {
    written = fwrite(data, 1, len, fp);
    if (written != len) {
      fclose(fp);
      return PLAT_ERR;
    }
  }

  fclose(fp);
  return PLAT_OK;
}

int plat_file_append(const char *path, const uint8_t *data, size_t len) {
  FILE *fp;
  size_t written;

  if (path == NULL || (data == NULL && len > 0)) {
    return PLAT_ERR;
  }

  fp = fopen(path, "ab");
  if (fp == NULL) {
    return PLAT_ERR;
  }

  if (len > 0) {
    written = fwrite(data, 1, len, fp);
    if (written != len) {
      fclose(fp);
      return PLAT_ERR;
    }
  }

  fclose(fp);
  return PLAT_OK;
}

int plat_file_rename(const char *old_path, const char *new_path) {
  if (old_path == NULL || new_path == NULL) {
    return PLAT_ERR;
  }

  /* rename() is atomic on POSIX when on same filesystem */
  if (rename(old_path, new_path) != 0) {
    return PLAT_ERR;
  }

  return PLAT_OK;
}

int plat_file_delete(const char *path) {
  if (path == NULL) {
    return PLAT_ERR;
  }

  if (unlink(path) != 0) {
    return PLAT_ERR;
  }

  return PLAT_OK;
}

int plat_file_exists(const char *path) {
  struct stat st;

  if (path == NULL) {
    return 0;
  }

  return (stat(path, &st) == 0) ? 1 : 0;
}

/*
 * Helper function to create directory with parents.
 * Similar to "mkdir -p".
 */
static int mkdir_recursive(const char *path) {
  char tmp[4096];
  char *p;
  size_t len;

  len = strlen(path);
  if (len == 0 || len >= sizeof(tmp)) {
    return PLAT_ERR;
  }

  memcpy(tmp, path, len + 1);

  /* Remove trailing slash if present */
  if (tmp[len - 1] == '/') {
    tmp[len - 1] = '\0';
  }

  /* Create each directory component */
  for (p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return PLAT_ERR;
      }
      *p = '/';
    }
  }

  /* Create final directory */
  if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
    return PLAT_ERR;
  }

  return PLAT_OK;
}

int plat_dir_create(const char *path) {
  if (path == NULL) {
    return PLAT_ERR;
  }

  return mkdir_recursive(path);
}

/*
 * ============================================================================
 * Time Implementation
 * ============================================================================
 */

uint64_t plat_time_ms(void) {
  struct timeval tv;

  if (gettimeofday(&tv, NULL) != 0) {
    return 0;
  }

  return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

uint64_t plat_monotonic_ms(void) {
  struct timespec ts;

  /*
   * CLOCK_MONOTONIC is available on all POSIX systems we care about.
   * It never goes backward and is not affected by NTP adjustments.
   */
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }

  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void plat_sleep_ms(uint32_t ms) {
  struct timespec req;
  struct timespec rem;

  req.tv_sec = ms / 1000;
  req.tv_nsec = (ms % 1000) * 1000000L;

  /* Handle interruptions by signal */
  while (nanosleep(&req, &rem) != 0 && errno == EINTR) {
    req = rem;
  }
}

/*
 * ============================================================================
 * Entropy Implementation
 * ============================================================================
 */

int plat_random_bytes(uint8_t *buf, size_t len) {
  if (buf == NULL || len == 0) {
    return PLAT_ERR;
  }

#if defined(HAVE_GETENTROPY)
  /*
   * getentropy() is available on macOS 10.12+ and some BSDs.
   * Maximum 256 bytes per call.
   */
  while (len > 0) {
    size_t chunk = (len > 256) ? 256 : len;
    if (getentropy(buf, chunk) != 0) {
      return PLAT_ERR;
    }
    buf += chunk;
    len -= chunk;
  }
  return PLAT_OK;

#elif defined(HAVE_GETRANDOM)
  /*
   * getrandom() is available on Linux 3.17+.
   * No size limit, but may block if entropy pool not initialized.
   */
  while (len > 0) {
    ssize_t ret = getrandom(buf, len, 0);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      return PLAT_ERR;
    }
    buf += ret;
    len -= (size_t)ret;
  }
  return PLAT_OK;

#else
  /*
   * Fallback: read from /dev/urandom.
   * Available on all Unix-like systems.
   */
  {
    int fd;
    ssize_t ret;

    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
      return PLAT_ERR;
    }

    while (len > 0) {
      ret = read(fd, buf, len);
      if (ret < 0) {
        if (errno == EINTR) {
          continue;
        }
        close(fd);
        return PLAT_ERR;
      }
      if (ret == 0) {
        /* Unexpected EOF */
        close(fd);
        return PLAT_ERR;
      }
      buf += ret;
      len -= (size_t)ret;
    }

    close(fd);
    return PLAT_OK;
  }
#endif
}
