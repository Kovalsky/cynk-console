#ifndef CYNK_DEVICE_NET_H
#define CYNK_DEVICE_NET_H

#include <stddef.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

typedef struct {
  const char *host;
  const char *port;
  const char *ca_path;
  int use_tls;
  int tls_insecure;
} cynk_net_config;

typedef struct cynk_mqtt_socket {
  int fd;
  int use_tls;
  mbedtls_net_context net;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_x509_crt ca;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
} cynk_mqtt_socket;

int cynk_net_connect(cynk_mqtt_socket **out, const cynk_net_config *cfg,
                     char *err, size_t err_cap);
void cynk_net_close(cynk_mqtt_socket *sock);

#endif
