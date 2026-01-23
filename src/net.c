#include "net.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <mbedtls/error.h>

static void set_error(char *err, size_t cap, const char *fmt, ...) {
  va_list args;
  if (!err || cap == 0) {
    return;
  }
  va_start(args, fmt);
  vsnprintf(err, cap, fmt, args);
  va_end(args);
}

static void set_mbedtls_error(char *err, size_t cap, const char *prefix, int code) {
  char buf[160];
  mbedtls_strerror(code, buf, sizeof(buf));
  set_error(err, cap, "%s: %s", prefix, buf);
}

static void init_tls(cynk_mqtt_socket *sock) {
  mbedtls_ssl_init(&sock->ssl);
  mbedtls_ssl_config_init(&sock->conf);
  mbedtls_x509_crt_init(&sock->ca);
  mbedtls_entropy_init(&sock->entropy);
  mbedtls_ctr_drbg_init(&sock->ctr_drbg);
}

int cynk_net_connect(cynk_mqtt_socket **out, const cynk_net_config *cfg,
                     char *err, size_t err_cap) {
  cynk_mqtt_socket *sock;
  int rc;
  int ret;

  if (!out || !cfg || !cfg->host || !cfg->port) {
    set_error(err, err_cap, "invalid network config");
    return -1;
  }

  sock = calloc(1, sizeof(*sock));
  if (!sock) {
    set_error(err, err_cap, "out of memory");
    return -1;
  }

  sock->use_tls = cfg->use_tls;
  mbedtls_net_init(&sock->net);

  if (cfg->use_tls) {
    const char *pers = "cynk-device";

    init_tls(sock);

    if (!cfg->tls_insecure && !cfg->ca_path) {
      set_error(err, err_cap, "TLS enabled but no CA provided");
      goto fail;
    }

    ret = mbedtls_ctr_drbg_seed(&sock->ctr_drbg, mbedtls_entropy_func, &sock->entropy,
                                (const unsigned char *)pers, strlen(pers));
    if (ret != 0) {
      set_mbedtls_error(err, err_cap, "mbedtls_ctr_drbg_seed failed", ret);
      goto fail;
    }

    if (!cfg->tls_insecure) {
      ret = mbedtls_x509_crt_parse_file(&sock->ca, cfg->ca_path);
      if (ret < 0) {
        set_mbedtls_error(err, err_cap, "mbedtls_x509_crt_parse_file failed", ret);
        goto fail;
      }
    }

    ret = mbedtls_net_connect(&sock->net, cfg->host, cfg->port, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
      set_mbedtls_error(err, err_cap, "mbedtls_net_connect failed", ret);
      goto fail;
    }

    ret = mbedtls_ssl_config_defaults(&sock->conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
      set_mbedtls_error(err, err_cap, "mbedtls_ssl_config_defaults failed", ret);
      goto fail;
    }

    mbedtls_ssl_conf_rng(&sock->conf, mbedtls_ctr_drbg_random, &sock->ctr_drbg);

    if (cfg->tls_insecure) {
      mbedtls_ssl_conf_authmode(&sock->conf, MBEDTLS_SSL_VERIFY_NONE);
    } else {
      mbedtls_ssl_conf_authmode(&sock->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
      mbedtls_ssl_conf_ca_chain(&sock->conf, &sock->ca, NULL);
    }

    ret = mbedtls_ssl_setup(&sock->ssl, &sock->conf);
    if (ret != 0) {
      set_mbedtls_error(err, err_cap, "mbedtls_ssl_setup failed", ret);
      goto fail;
    }

    ret = mbedtls_ssl_set_hostname(&sock->ssl, cfg->host);
    if (ret != 0) {
      set_mbedtls_error(err, err_cap, "mbedtls_ssl_set_hostname failed", ret);
      goto fail;
    }

    mbedtls_ssl_set_bio(&sock->ssl, &sock->net, mbedtls_net_send, mbedtls_net_recv,
                        NULL);

    while ((ret = mbedtls_ssl_handshake(&sock->ssl)) != 0) {
      if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        continue;
      }
      set_mbedtls_error(err, err_cap, "mbedtls_ssl_handshake failed", ret);
      goto fail;
    }

    if (!cfg->tls_insecure) {
      uint32_t flags = mbedtls_ssl_get_verify_result(&sock->ssl);
      if (flags != 0) {
        char info[160];
        mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
        set_error(err, err_cap, "TLS verify failed: %s", info);
        goto fail;
      }
    }

    ret = mbedtls_net_set_nonblock(&sock->net);
    if (ret != 0) {
      set_mbedtls_error(err, err_cap, "mbedtls_net_set_nonblock failed", ret);
      goto fail;
    }
  } else {
    rc = mbedtls_net_connect(&sock->net, cfg->host, cfg->port, MBEDTLS_NET_PROTO_TCP);
    if (rc != 0) {
      set_mbedtls_error(err, err_cap, "mbedtls_net_connect failed", rc);
      goto fail;
    }

    rc = mbedtls_net_set_nonblock(&sock->net);
    if (rc != 0) {
      set_mbedtls_error(err, err_cap, "mbedtls_net_set_nonblock failed", rc);
      goto fail;
    }
  }

  sock->fd = sock->net.fd;
  *out = sock;
  return 0;

fail:
  cynk_net_close(sock);
  return -1;
}

void cynk_net_close(cynk_mqtt_socket *sock) {
  if (!sock) {
    return;
  }

  if (sock->use_tls) {
    mbedtls_ssl_close_notify(&sock->ssl);
    mbedtls_ssl_free(&sock->ssl);
    mbedtls_ssl_config_free(&sock->conf);
    mbedtls_x509_crt_free(&sock->ca);
    mbedtls_ctr_drbg_free(&sock->ctr_drbg);
    mbedtls_entropy_free(&sock->entropy);
  }

  mbedtls_net_free(&sock->net);
  free(sock);
}
