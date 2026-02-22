/*
 * compat.h — Platform abstraction layer for cynk-console.
 *
 * Provides a unified API over POSIX and Win32 for threading, terminal I/O,
 * time, file paths, sockets, and byte-order operations. On POSIX systems the
 * implementations live in compat_posix.c; on Windows they live in
 * compat_win32.c. Only one of the two is compiled into the final binary
 * (selected by CMake).
 */
#ifndef CYNK_COMPAT_H
#define CYNK_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/*  Platform detection & low-level includes                           */
/* ------------------------------------------------------------------ */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <basetsd.h>
#include <io.h>

typedef SSIZE_T ssize_t;

/* Mutex: SRWLOCK has a static initialiser (Vista+). */
typedef struct {
  SRWLOCK lock;
} compat_mutex_t;
#define COMPAT_MUTEX_INITIALIZER \
  { SRWLOCK_INIT }

/* Thread handle. */
typedef HANDLE compat_thread_t;

/* Terminal state saved/restored around raw mode. */
typedef struct {
  DWORD in_mode;
  DWORD out_mode;
  HANDLE hin;
  HANDLE hout;
} compat_term_state;

#else /* POSIX */

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
  pthread_mutex_t lock;
} compat_mutex_t;
#define COMPAT_MUTEX_INITIALIZER \
  { PTHREAD_MUTEX_INITIALIZER }

typedef pthread_t compat_thread_t;

typedef struct {
  struct termios orig;
} compat_term_state;

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/*  Inline mutex operations (perf-sensitive, used in hot paths)       */
/* ------------------------------------------------------------------ */

static inline int compat_mutex_init(compat_mutex_t *m) {
#ifdef _WIN32
  InitializeSRWLock(&m->lock);
  return 0;
#else
  return pthread_mutex_init(&m->lock, NULL);
#endif
}

static inline void compat_mutex_lock(compat_mutex_t *m) {
#ifdef _WIN32
  AcquireSRWLockExclusive(&m->lock);
#else
  pthread_mutex_lock(&m->lock);
#endif
}

static inline void compat_mutex_unlock(compat_mutex_t *m) {
#ifdef _WIN32
  ReleaseSRWLockExclusive(&m->lock);
#else
  pthread_mutex_unlock(&m->lock);
#endif
}

/* ------------------------------------------------------------------ */
/*  Inline byte-order helpers                                         */
/* ------------------------------------------------------------------ */

static inline uint16_t compat_htons(uint16_t v) {
  return htons(v);
}

static inline uint16_t compat_ntohs(uint16_t v) {
  return ntohs(v);
}

/* ------------------------------------------------------------------ */
/*  One-time console initialisation (enables ANSI on Windows)         */
/* ------------------------------------------------------------------ */

void compat_console_init(void);

/* ------------------------------------------------------------------ */
/*  Threading                                                         */
/* ------------------------------------------------------------------ */

int compat_thread_create(compat_thread_t *t, void *(*fn)(void *), void *arg);
int compat_thread_join(compat_thread_t t);

/* ------------------------------------------------------------------ */
/*  Terminal I/O                                                      */
/* ------------------------------------------------------------------ */

int  compat_isatty(int fd);
int  compat_term_raw_enable(compat_term_state *state);
void compat_term_raw_disable(const compat_term_state *state);
int  compat_read_char(unsigned char *c);

/* ------------------------------------------------------------------ */
/*  Time                                                              */
/* ------------------------------------------------------------------ */

void     compat_sleep_ms(unsigned int ms);
uint64_t compat_now_ms(void);
int      compat_gmtime(const time_t *t, struct tm *out);

/* ------------------------------------------------------------------ */
/*  Paths                                                             */
/* ------------------------------------------------------------------ */

const char *compat_home_dir(void);
const char *compat_default_ca_path(void);

/* ------------------------------------------------------------------ */
/*  Sockets (thin wrappers for send/recv + error check)               */
/* ------------------------------------------------------------------ */

ssize_t compat_socket_send(int fd, const void *buf, size_t len, int flags);
ssize_t compat_socket_recv(int fd, void *buf, size_t len, int flags);
int     compat_socket_would_block(void);

#endif /* CYNK_COMPAT_H */
