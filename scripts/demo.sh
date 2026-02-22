#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <device_id> <device_password> [slug] [value]" >&2
  exit 1
fi

device_id=$1
password=$2
slug=${3:-slider_1}
value=${4:-42}

broker=${BROKER:-staging.cynk.tech}
port=${PORT:-8883}
handshake_timeout=${HANDSHAKE_TIMEOUT:-10000}
tls=${TLS:-1}
tls_ca=${TLS_CA:-/etc/ssl/certs/ca-certificates.crt}
tls_insecure=${TLS_INSECURE:-0}

args=(
  --device-id "$device_id"
  --password "$password"
  --broker "$broker"
  --port "$port"
  --handshake-timeout "$handshake_timeout"
)

if [[ "$tls" == "1" ]]; then
  if [[ -n "$tls_ca" ]]; then
    args+=(--tls-ca "$tls_ca")
  elif [[ "$tls_insecure" == "1" ]]; then
    args+=(--tls-insecure)
  else
    echo "TLS=1 requires TLS_CA or TLS_INSECURE=1" >&2
    exit 1
  fi
else
  args+=(--no-tls)
fi

printf "connect\nsend slug=%s %s\nquit\n" "$slug" "$value" | ./build/cynk-console "${args[@]}"
