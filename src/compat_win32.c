/*
 * compat_win32.c — Windows implementation of the platform abstraction layer.
 *
 * Wraps the Win32 Console API, Windows threads, Winsock2, and
 * QueryPerformanceCounter/GetSystemTimeAsFileTime behind the compat_*
 * interface defined in compat.h.
 */
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Console init — enable ANSI/VT processing on stdout                */
/* ------------------------------------------------------------------ */

void compat_console_init(void) {
  HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;

  if (hout != INVALID_HANDLE_VALUE && GetConsoleMode(hout, &mode)) {
    SetConsoleMode(hout, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  }
}

/* ------------------------------------------------------------------ */
/*  Threading                                                         */
/* ------------------------------------------------------------------ */

struct compat_thread_trampoline {
  void *(*fn)(void *);
  void *arg;
};

static DWORD WINAPI compat_thread_entry(LPVOID param) {
  struct compat_thread_trampoline t = *(struct compat_thread_trampoline *)param;
  free(param);
  t.fn(t.arg);
  return 0;
}

int compat_thread_create(compat_thread_t *t, void *(*fn)(void *), void *arg) {
  struct compat_thread_trampoline *tramp;

  tramp = malloc(sizeof(*tramp));
  if (!tramp) {
    return -1;
  }

  tramp->fn = fn;
  tramp->arg = arg;
  *t = CreateThread(NULL, 0, compat_thread_entry, tramp, 0, NULL);
  if (!*t) {
    free(tramp);
    return -1;
  }

  return 0;
}

int compat_thread_join(compat_thread_t t) {
  if (!t) {
    return -1;
  }
  WaitForSingleObject(t, INFINITE);
  CloseHandle(t);
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Terminal I/O                                                      */
/* ------------------------------------------------------------------ */

int compat_isatty(int fd) {
  return _isatty(fd);
}

int compat_term_raw_enable(compat_term_state *state) {
  state->hin = GetStdHandle(STD_INPUT_HANDLE);
  state->hout = GetStdHandle(STD_OUTPUT_HANDLE);

  if (state->hin == INVALID_HANDLE_VALUE) {
    return -1;
  }

  if (!GetConsoleMode(state->hin, &state->in_mode)) {
    return -1;
  }

  if (state->hout != INVALID_HANDLE_VALUE) {
    GetConsoleMode(state->hout, &state->out_mode);
  }

  /* Disable line buffering and echo, enable VT input sequences. */
  DWORD raw_in = state->in_mode;
  raw_in &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
  raw_in |= ENABLE_VIRTUAL_TERMINAL_INPUT;

  if (!SetConsoleMode(state->hin, raw_in)) {
    return -1;
  }

  return 0;
}

void compat_term_raw_disable(const compat_term_state *state) {
  if (state->hin != INVALID_HANDLE_VALUE) {
    SetConsoleMode(state->hin, state->in_mode);
  }
}

int compat_read_char(unsigned char *c) {
  HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
  char buf;
  DWORD nread;

  if (!ReadConsoleA(hin, &buf, 1, &nread, NULL) || nread == 0) {
    return -1;
  }

  *c = (unsigned char)buf;
  return 1;
}

/* ------------------------------------------------------------------ */
/*  Time                                                              */
/* ------------------------------------------------------------------ */

void compat_sleep_ms(unsigned int ms) {
  Sleep((DWORD)ms);
}

uint64_t compat_now_ms(void) {
  FILETIME ft;
  ULARGE_INTEGER uli;

  GetSystemTimeAsFileTime(&ft);
  uli.LowPart = ft.dwLowDateTime;
  uli.HighPart = ft.dwHighDateTime;

  /* Convert 100-ns intervals since 1601-01-01 → ms since Unix epoch. */
  return (uint64_t)((uli.QuadPart - 116444736000000000ULL) / 10000ULL);
}

int compat_gmtime(const time_t *t, struct tm *out) {
  return gmtime_s(out, t) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Paths                                                             */
/* ------------------------------------------------------------------ */

const char *compat_home_dir(void) {
  const char *appdata = getenv("APPDATA");
  if (appdata && appdata[0]) {
    return appdata;
  }
  return getenv("USERPROFILE");
}

const char *compat_default_ca_path(void) {
  /*
   * Windows has no standard CA bundle file. Users must supply --tls-ca <path>
   * pointing at a PEM bundle, or use --tls-insecure for testing.
   */
  return NULL;
}

/* ------------------------------------------------------------------ */
/*  Sockets                                                           */
/* ------------------------------------------------------------------ */

ssize_t compat_socket_send(int fd, const void *buf, size_t len, int flags) {
  return send((SOCKET)fd, (const char *)buf, (int)len, flags);
}

ssize_t compat_socket_recv(int fd, void *buf, size_t len, int flags) {
  return recv((SOCKET)fd, (char *)buf, (int)len, flags);
}

int compat_socket_would_block(void) {
  return WSAGetLastError() == WSAEWOULDBLOCK;
}
