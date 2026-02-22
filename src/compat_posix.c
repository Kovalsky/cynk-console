/*
 * compat_posix.c — POSIX implementation of the platform abstraction layer.
 *
 * Wraps pthread, termios, clock_gettime, BSD sockets, and standard POSIX APIs
 * behind the compat_* interface defined in compat.h.
 */
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Console init (no-op on POSIX — terminals handle ANSI natively)    */
/* ------------------------------------------------------------------ */

void compat_console_init(void) {
  /* Nothing to do. */
}

/* ------------------------------------------------------------------ */
/*  Threading                                                         */
/* ------------------------------------------------------------------ */

int compat_thread_create(compat_thread_t *t, void *(*fn)(void *), void *arg) {
  return pthread_create(t, NULL, fn, arg);
}

int compat_thread_join(compat_thread_t t) {
  return pthread_join(t, NULL);
}

/* ------------------------------------------------------------------ */
/*  Terminal I/O                                                      */
/* ------------------------------------------------------------------ */

int compat_isatty(int fd) {
  return isatty(fd);
}

int compat_term_raw_enable(compat_term_state *state) {
  struct termios raw;

  if (tcgetattr(STDIN_FILENO, &state->orig) != 0) {
    return -1;
  }

  raw = state->orig;
  raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
  raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
    return -1;
  }

  return 0;
}

void compat_term_raw_disable(const compat_term_state *state) {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &state->orig);
}

int compat_read_char(unsigned char *c) {
  ssize_t n = read(STDIN_FILENO, c, 1);
  return n == 1 ? 1 : -1;
}

/* ------------------------------------------------------------------ */
/*  Time                                                              */
/* ------------------------------------------------------------------ */

void compat_sleep_ms(unsigned int ms) {
  usleep(ms * 1000u);
}

uint64_t compat_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

int compat_gmtime(const time_t *t, struct tm *out) {
  return gmtime_r(t, out) != NULL ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Paths                                                             */
/* ------------------------------------------------------------------ */

const char *compat_home_dir(void) {
  return getenv("HOME");
}

const char *compat_default_ca_path(void) {
  return "/etc/ssl/certs/ca-certificates.crt";
}

/* ------------------------------------------------------------------ */
/*  Sockets                                                           */
/* ------------------------------------------------------------------ */

ssize_t compat_socket_send(int fd, const void *buf, size_t len, int flags) {
  return send(fd, buf, len, flags);
}

ssize_t compat_socket_recv(int fd, void *buf, size_t len, int flags) {
  return recv(fd, buf, len, flags);
}

int compat_socket_would_block(void) {
  return errno == EAGAIN || errno == EWOULDBLOCK;
}
