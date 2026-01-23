#include <mqtt.h>

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/ssl.h>

#include "net.h"

static ssize_t mqtt_pal_sendall_plain(cynk_mqtt_socket *sock, const void *buf,
                                      size_t len, int flags) {
  size_t sent = 0;

  while (sent < len) {
    ssize_t rv = send(sock->fd, (const char *)buf + sent, len - sent, flags);
    if (rv < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      return MQTT_ERROR_SOCKET_ERROR;
    }
    if (rv == 0) {
      break;
    }
    sent += (size_t)rv;
  }

  return sent == 0 ? 0 : (ssize_t)sent;
}

static ssize_t mqtt_pal_recvall_plain(cynk_mqtt_socket *sock, void *buf,
                                      size_t bufsz, int flags) {
  void *start = buf;

  while (bufsz > 0) {
    ssize_t rv = recv(sock->fd, buf, bufsz, flags);
    if (rv < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      return MQTT_ERROR_SOCKET_ERROR;
    }
    if (rv == 0) {
      return MQTT_ERROR_SOCKET_ERROR;
    }
    buf = (char *)buf + rv;
    bufsz -= (size_t)rv;
  }

  return buf == start ? 0 : (ssize_t)((char *)buf - (char *)start);
}

ssize_t mqtt_pal_sendall(mqtt_pal_socket_handle fd, const void *buf, size_t len,
                         int flags) {
  cynk_mqtt_socket *sock = fd;
  enum MQTTErrors error = 0;
  size_t sent = 0;

  if (!sock) {
    return MQTT_ERROR_SOCKET_ERROR;
  }

  if (!sock->use_tls) {
    return mqtt_pal_sendall_plain(sock, buf, len, flags);
  }

  while (sent < len) {
    int rv =
        mbedtls_ssl_write(&sock->ssl, (const unsigned char *)buf + sent, len - sent);
    if (rv < 0) {
      if (rv == MBEDTLS_ERR_SSL_WANT_READ || rv == MBEDTLS_ERR_SSL_WANT_WRITE
#if defined(MBEDTLS_ERR_SSL_ASYNC_IN_PROGRESS)
          || rv == MBEDTLS_ERR_SSL_ASYNC_IN_PROGRESS
#endif
#if defined(MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS)
          || rv == MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS
#endif
      ) {
        break;
      }
      error = MQTT_ERROR_SOCKET_ERROR;
      break;
    }
    sent += (size_t)rv;
  }

  if (sent == 0) {
    return error;
  }
  return (ssize_t)sent;
}

ssize_t mqtt_pal_recvall(mqtt_pal_socket_handle fd, void *buf, size_t bufsz,
                         int flags) {
  cynk_mqtt_socket *sock = fd;
  void *start = buf;
  enum MQTTErrors error = 0;
  int rv;

  if (!sock) {
    return MQTT_ERROR_SOCKET_ERROR;
  }

  if (!sock->use_tls) {
    return mqtt_pal_recvall_plain(sock, buf, bufsz, flags);
  }

  do {
    rv = mbedtls_ssl_read(&sock->ssl, (unsigned char *)buf, bufsz);
    if (rv == 0) {
      error = MQTT_ERROR_SOCKET_ERROR;
      break;
    }
    if (rv < 0) {
      if (rv == MBEDTLS_ERR_SSL_WANT_READ || rv == MBEDTLS_ERR_SSL_WANT_WRITE
#if defined(MBEDTLS_ERR_SSL_ASYNC_IN_PROGRESS)
          || rv == MBEDTLS_ERR_SSL_ASYNC_IN_PROGRESS
#endif
#if defined(MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS)
          || rv == MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS
#endif
      ) {
        break;
      }
      error = MQTT_ERROR_SOCKET_ERROR;
      break;
    }
    buf = (char *)buf + rv;
    bufsz -= (size_t)rv;
  } while (bufsz > 0);

  if (buf == start) {
    return error;
  }
  return (ssize_t)((char *)buf - (char *)start);
}
