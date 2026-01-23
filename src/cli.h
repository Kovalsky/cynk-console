#ifndef CYNK_DEVICE_CLI_H
#define CYNK_DEVICE_CLI_H

struct cli_config {
  const char *device_id;
  const char *password;
  const char *broker;
  int port;
  int qos;
  int keepalive;
  int handshake_timeout_ms;
  int tls;
  const char *tls_ca;
  int tls_insecure;
};

int cli_run(const struct cli_config *cfg);

#endif
