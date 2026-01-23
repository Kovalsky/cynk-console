# Cynk Device CLI

Interactive terminal emulator for Cynk devices. This CLI reuses the C SDK to run the status handshake, publish telemetry, and print incoming commands.

## Build

```bash
cmake -S . -B build -DCYNK_DEVICE_SDK_DIR=../cynk-sdk-c
cmake --build build
```

## Run

TLS is on by default (port 8883). A dev CA is bundled at `certs/dev_ca.crt`.

```bash
./build/cynk-device \
  --device-id <device_id> \
  --password <device_password> \
  --broker localhost
```

Plain TCP:

```bash
./build/cynk-device --device-id <device_id> --password <device_password> --no-tls --port 1883
```

## Shell Commands

- `connect` - connect, publish `status=online`, wait for the status ack.
- `send <slug> <value>` - send telemetry for a widget slug.
- `send <id> <value>` - send telemetry for a widget id (uuid only).
- `send slug=<slug> <value>` - send telemetry for a widget slug.
- `send id=<id> <value>` - send telemetry for a widget id.
- `send slug=<slug> value=<value>` - named value form.

Example:
```text
send slug=chart_2 23
```
- `raw <json>` - send a raw telemetry document.
- `profile sensor|controller|hybrid|custom` - set output profile.
- `status` - show connection/handshake state and last publish summary.
- `help` - list commands.
- `quit` / `exit` - publish `status=offline` and exit.

## Quick Demo (Non-interactive)

```bash
printf "connect\nsend <slug> 42\nquit\n" | \
  ./build/cynk-device --device-id <device_id> --password <device_password> --no-tls --port 1883
```

## Demo Script

```bash
./scripts/demo.sh <device_id> <device_password> [slug] [value]
```

## Notes

- The CLI subscribes to `cynk/v1/+/{device_id}/command` and prints commands based on the active profile.
- Telemetry is sent only after the handshake ack provides `user_id`.
- `send` publishes a single `value` with an auto-generated `ts`.
- Raw payloads are sent as-is; the backend is responsible for validation.
- Command history is available with ↑/↓ and saved to `~/.cynk-device_history` (override with `CYNK_DEVICE_HISTORY`).
- Press Tab for command suggestions (commands, profiles, `send` options).
- Colors are on by default; set `CYNK_DEVICE_NO_COLOR=1` to disable.
- A dev CA is bundled at `certs/dev_ca.crt` so TLS works out of the box for local brokers.

## Troubleshooting

- Handshake timeout: ensure the backend consumer is running and subscribed to `cynk/v1/status/+`.
- TLS CA missing: for the dev broker, regenerate `priv/dev_tls/ca.crt` with `./scripts/gen_dev_mqtt_certs.sh` in the main Cynk repo, or use `--tls-insecure` for local tests.
- TLS CA path errors: run the CLI from the repo root (so `certs/dev_ca.crt` resolves) or pass `--tls-ca /path/to/ca.crt`.
