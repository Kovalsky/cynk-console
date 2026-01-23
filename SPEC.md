# Cynk Device CLI - Specification

## Goals
- Provide an interactive terminal emulator (`cynk-device`) for Cynk devices.
- Reuse the C SDK protocol: status handshake, telemetry with widget refs, command handling.
- Offer sensor/controller/hybrid profiles as shortcuts without restricting custom flows.
- Ship as a single C binary built with CMake + MQTT-C + mbedTLS.

## Handshake Flow
1. MQTT client connects with `client_id = device_id` and `username = device_id`.
2. LWT is configured on `cynk/v1/status/{device_id}` with payload `{ "status": "offline", ... }`.
3. On connect the CLI subscribes to:
   - `cynk/v1/status/{device_id}/ack` to learn `user_id` + topics.
   - `cynk/v1/+/{device_id}/command` to capture commands.
4. The CLI publishes `status=online` on the status topic, waits for the ack before sending telemetry.
5. TLS is on by default (port 8883). Use `--no-tls` for plaintext or `--tls-ca` / `--tls-insecure` for TLS settings.

## Shell Command Set
- `connect` - start MQTT client, publish online status, wait for ack.
- `send slug=<widget> <value>` / `send id=<widget> <value>` - publish telemetry value with auto `ts`.
- `raw <json>` - publish a raw telemetry document.
- `profile <name>` - switch between `sensor`, `controller`, `hybrid`, `custom`.
- `status` - inspect connection/handshake state and last publish summary.
- `quit/exit` - publish offline status and exit.

## Profiles
- `sensor` - hides incoming commands while still performing the handshake.
- `controller` - shows commands and encourages command handling.
- `hybrid` - both telemetry and command payloads are visible.
- `custom` - no shortcuts; the user controls the flow entirely.

Profiles only affect CLI output. All commands remain available so users can script custom behavior.

## Error Handling
- `send`/`raw` fail if the handshake is not ready.
- Raw payloads are sent as-is; telemetry validation happens on the backend.
