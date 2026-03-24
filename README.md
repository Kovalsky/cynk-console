# Cynk Console

Interactive terminal emulator for Cynk devices. This CLI reuses the C SDK to run the status handshake, publish telemetry, and print incoming commands.

## Install

### macOS (Homebrew)

```bash
brew install cynk/cynk/cynk-console
```

### Linux

```bash
curl -fsSL https://raw.githubusercontent.com/Kovalsky/cynk-console/master/scripts/install.sh | sh
```

Or specify a version:

```bash
CYNK_VERSION=v0.2.0 curl -fsSL https://raw.githubusercontent.com/Kovalsky/cynk-console/master/scripts/install.sh | sh
```

### Windows

Download `cynk-console-setup-x86_64.exe` from the [latest release](https://github.com/Kovalsky/cynk-console/releases/latest) and run the installer.

Or download `cynk-console-windows-x86_64.exe` directly and add it to your PATH.

### From Source

```bash
git clone --recursive https://github.com/Kovalsky/cynk-console.git
cd cynk-console
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Run

TLS is on by default (port 8883). Connects to `staging.cynk.tech` using the system CA bundle.

```bash
cynk-console --device-id <device_id> --password <device_password>
```

Local dev (plain TCP):

```bash
cynk-console --device-id <device_id> --password <device_password> --broker localhost --no-tls --port 1883
```

Local dev (TLS with dev CA):

```bash
cynk-console --device-id <device_id> --password <device_password> --broker localhost --tls-ca certs/dev_ca.crt
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
  cynk-console --device-id <device_id> --password <device_password>
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
- Command history is available with ↑/↓ and saved to `~/.cynk-console_history` (override with `CYNK_CONSOLE_HISTORY`).
- Press Tab for command suggestions (commands, profiles, `send` options).
- Colors are on by default; set `CYNK_CONSOLE_NO_COLOR=1` to disable.
- A dev CA is bundled at `certs/dev_ca.crt` for local broker testing.

## Troubleshooting

- **Handshake timeout**: ensure the backend consumer is running and subscribed to `cynk/v1/status/+`.
- **TLS CA missing**: for the dev broker, regenerate `priv/dev_tls/ca.crt` with `./scripts/gen_dev_mqtt_certs.sh` in the main Cynk repo, or use `--tls-insecure` for local tests.
- **TLS CA not auto-detected**: the tool probes standard system paths (`/etc/ssl/cert.pem`, `/etc/ssl/certs/ca-certificates.crt`, etc.). If none match, use `--tls-ca /path/to/ca-bundle.crt` or `--tls-insecure` for testing.
- **macOS Gatekeeper blocks the binary**: run `xattr -d com.apple.quarantine $(which cynk-console)` to remove the quarantine attribute.
