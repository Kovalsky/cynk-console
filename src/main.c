#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog) {
  printf("Usage: %s --device-id <id> --password <password> [options]\n", prog);
  puts("");
  puts("Options:");
  puts("  --broker <host>            MQTT broker host (default: localhost)");
  puts("  --port <port>              MQTT broker port (default: 8883 for TLS, 1883 for no TLS)");
  puts("  --no-tls                   disable TLS");
  puts("  --tls-ca <path>            CA cert path for TLS");
  puts("  --tls-insecure             skip TLS verification");
  puts("  --qos <0|1|2>              publish QoS (default: 1)");
  puts("  --keepalive <seconds>      MQTT keepalive (default: 30)");
  puts("  --handshake-timeout <ms>   handshake timeout (default: 10000)");
  puts("  --help                     show this help");
}

static int parse_int(const char *arg, int *out) {
  char *end = NULL;
  long value = strtol(arg, &end, 10);

  if (!end || *end != '\0') {
    return -1;
  }
  *out = (int)value;
  return 0;
}

int main(int argc, char **argv) {
  struct cli_config cfg;
  int i;

  memset(&cfg, 0, sizeof(cfg));
  cfg.broker = "localhost";
  cfg.qos = 1;
  cfg.keepalive = 30;
  cfg.handshake_timeout_ms = 10000;
  cfg.tls = 1;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--device-id") == 0 && i + 1 < argc) {
      cfg.device_id = argv[++i];
    } else if (strcmp(argv[i], "--password") == 0 && i + 1 < argc) {
      cfg.password = argv[++i];
    } else if (strcmp(argv[i], "--broker") == 0 && i + 1 < argc) {
      cfg.broker = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      if (parse_int(argv[++i], &cfg.port) != 0) {
        fprintf(stderr, "invalid port\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--qos") == 0 && i + 1 < argc) {
      if (parse_int(argv[++i], &cfg.qos) != 0) {
        fprintf(stderr, "invalid qos\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--keepalive") == 0 && i + 1 < argc) {
      if (parse_int(argv[++i], &cfg.keepalive) != 0) {
        fprintf(stderr, "invalid keepalive\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--handshake-timeout") == 0 && i + 1 < argc) {
      if (parse_int(argv[++i], &cfg.handshake_timeout_ms) != 0) {
        fprintf(stderr, "invalid handshake timeout\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--tls-ca") == 0 && i + 1 < argc) {
      cfg.tls_ca = argv[++i];
    } else if (strcmp(argv[i], "--tls-insecure") == 0) {
      cfg.tls_insecure = 1;
    } else if (strcmp(argv[i], "--no-tls") == 0) {
      cfg.tls = 0;
    } else if (strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "unknown option: %s\n", argv[i]);
      print_usage(argv[0]);
      return 1;
    }
  }

  if (!cfg.device_id || !cfg.password) {
    print_usage(argv[0]);
    return 1;
  }

  if (cfg.port == 0) {
    cfg.port = cfg.tls ? 8883 : 1883;
  }

  if (cfg.port <= 0) {
    fprintf(stderr, "invalid port\n");
    return 1;
  }

  if (cfg.qos < 0 || cfg.qos > 2) {
    fprintf(stderr, "qos must be 0, 1, or 2\n");
    return 1;
  }

  if (cfg.keepalive <= 0) {
    fprintf(stderr, "keepalive must be positive\n");
    return 1;
  }

  if (cfg.handshake_timeout_ms <= 0) {
    fprintf(stderr, "handshake timeout must be positive\n");
    return 1;
  }

  if (cfg.tls && !cfg.tls_ca && !cfg.tls_insecure) {
    fprintf(stderr, "TLS enabled but no CA provided. Use --tls-ca or --tls-insecure.\n");
    return 1;
  }

  return cli_run(&cfg);
}
