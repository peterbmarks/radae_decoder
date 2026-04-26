# FreeDV Reporter WebSocket API

This document describes the Socket.IO-based API for writing alternative clients to the FreeDV Reporter server. From: `https://bitbucket.org/tmiw/freedv-reporter/src/master/`

## Transport

- **Protocol**: Socket.IO v4 (over WebSocket)
- **Default endpoint**: `http://127.0.0.1:8080`
- **Socket.IO path**: `/socket.io/` (default)
- **CORS**: All origins allowed

## Connection & Authentication

Authentication is passed in the Socket.IO `auth` object at connect time. The server will refuse the connection if required fields are missing or invalid.

### Roles

| Role | Description |
|---|---|
| `view` | Read-only observer (web browser, alternative display client) |
| `report` | FreeDV station that reports and can see others |
| `report_wo` | FreeDV station that reports but cannot view others (e.g. ezDV) |

### Auth fields

| Field | Type | Required for | Description |
|---|---|---|---|
| `role` | string | all | One of `view`, `report`, `report_wo` |
| `callsign` | string | report, report_wo | Valid amateur callsign |
| `grid_square` | string | report, report_wo | Maidenhead grid locator (non-empty) |
| `version` | string | report, report_wo | FreeDV app version string (non-empty) |
| `protocol_version` | integer | optional | `1` or `2` (default: `1`). Version 2 enables bulk updates. |
| `rx_only` | boolean | optional | Station is receive-only |
| `os` | string | optional | `windows`, `linux`, `macos`, or empty string |

**View client example:**
```json
{
  "role": "view",
  "protocol_version": 2
}
```

**Reporting station example:**
```json
{
  "role": "report",
  "callsign": "N0CALL",
  "grid_square": "DM79",
  "version": "1.4.8",
  "protocol_version": 2,
  "rx_only": false,
  "os": "linux"
}
```

On successful connection the server emits `connection_successful` to the connecting client (no data payload).

### Callsign validation regex

```
^(([A-Za-z0-9]+/)?[A-Za-z0-9]{1,3}[0-9][A-Za-z0-9]*[A-Za-z](/[A-Za-z0-9]+)?)$
```

---

## Server → Client Events

All timestamps are ISO 8601 UTC strings (e.g. `"2026-04-18T16:30:45.123456+00:00"`).

The `sid` field in every message is the Socket.IO session ID of the station being described. Use it as a stable key to associate subsequent updates with the same station.

---

### `new_connection`

A reporting station has connected.

```json
{
  "sid": "abc123",
  "callsign": "N0CALL",
  "grid_square": "DM79",
  "version": "1.4.8",
  "rx_only": false,
  "os": "linux",
  "last_update": "2026-04-18T16:30:45.123456+00:00",
  "connect_time": "2026-04-18T16:30:00.000000+00:00"
}
```

---

### `remove_connection`

A reporting station has disconnected or hidden itself.

Same structure as `new_connection`.

---

### `freq_change`

A station has changed frequency.

```json
{
  "sid": "abc123",
  "callsign": "N0CALL",
  "grid_square": "DM79",
  "freq": 14236000,
  "last_update": "2026-04-18T16:31:15.456789+00:00"
}
```

`freq` is in Hz.

---

### `tx_report`

A station's transmit state or mode has changed.

```json
{
  "sid": "abc123",
  "callsign": "N0CALL",
  "grid_square": "DM79",
  "mode": "700D",
  "transmitting": true,
  "last_tx": "2026-04-18T16:31:45.123456+00:00",
  "last_update": "2026-04-18T16:31:45.123456+00:00"
}
```

`last_tx` is null if the station has never transmitted this session.

---

### `rx_report`

A station has received a signal (or cleared its RX state after a frequency change).

```json
{
  "sid": "abc123",
  "callsign": "W5ABC",
  "snr": 8,
  "mode": "700D",
  "receiver_callsign": "N0CALL",
  "receiver_grid_square": "DM79",
  "last_update": "2026-04-18T16:32:00.789123+00:00"
}
```

When a station changes frequency the server sends a clearing `rx_report` with `callsign: ""`, `snr: 0`, and `mode: ""` for that station's `sid`.

---

### `message_update`

A station has changed its freeform status message.

```json
{
  "sid": "abc123",
  "message": "Looking for contacts",
  "last_update": "2026-04-18T16:32:30.345678+00:00"
}
```

`message` may be an empty string.

---

### `qsy_request`

Another station is requesting you move to a different frequency. Sent only to the targeted station.

```json
{
  "callsign": "W5ABC",
  "frequency": 7177000,
  "message": "Let's move to 7.177"
}
```

`frequency` is in Hz.

---

### `chat_message`

A chat message posted by a user.

```json
{
  "sentDate": "2026-04-18T16:33:00.123456+00:00",
  "username": "N0CALL",
  "isMeMessage": false,
  "content": "Anyone on 14.236?"
}
```

If a message was sent with a `/me ` prefix the prefix is stripped and `isMeMessage` is `true`.

---

### `chat_login`

A user has logged into chat.

```json
{
  "callsign": "N0CALL"
}
```

---

### `chat_logout`

A user has disconnected from chat.

```json
{
  "callsign": "N0CALL"
}
```

---

### `bulk_update`

Only sent to clients that connected with `protocol_version: 2`. Batches multiple events into a single message to reduce overhead. The payload is an array of `[event_name, event_data]` pairs.

```json
[
  ["new_connection", { "sid": "abc123", "callsign": "N0CALL", ... }],
  ["freq_change",    { "sid": "abc123", "freq": 14236000, ... }],
  ["rx_report",      { "sid": "abc123", "callsign": "W5ABC", ... }]
]
```

Each item in the array has the same structure as the corresponding individual event listed above. You should handle `bulk_update` by dispatching each tuple to the same handlers you use for individual events.

---

## Client → Server Events

These are only valid for the listed roles. Invalid requests are silently ignored.

---

### `freq_change` — roles: report, report_wo

```json
{ "freq": 14236000 }
```

Clears any active RX report and broadcasts a `freq_change` event to all listeners.

---

### `tx_report` — roles: report, report_wo

```json
{
  "mode": "700D",
  "transmitting": true
}
```

---

### `rx_report` — roles: report, report_wo

```json
{
  "callsign": "W5ABC",
  "snr": 8,
  "mode": "700D"
}
```

Rate-limited to at most once every 2 seconds per station. Updates are batched and flushed to listeners approximately every 150 ms.

---

### `message_update` — roles: report, report_wo

```json
{ "message": "Looking for contacts" }
```

Empty string clears the message.

---

### `hide_self` — role: report

No data payload. Removes the station from all listeners' views without disconnecting. The station can still receive updates.

---

### `show_self` — role: report

No data payload. Reverses `hide_self`. The server re-broadcasts the station's current `freq_change`, `tx_report`, and `message_update` to all listeners.

---

### `chat_login` — role: view

```json
{ "callsign": "N0CALL" }
```

Callsign must pass the validation regex above.

---

### `chat_message` — role: view (after chat_login)

```json
{ "message": "Anyone on 14.236?" }
```

Prefix with `/me ` for an action-style message. Messages are stored in SQLite for 14 days and optionally forwarded to Discord if the server is configured for it.

---

### `qsy_request` — role: report

```json
{
  "dest_sid": "target-socket-id",
  "frequency": 7177000,
  "message": "Let's move to 7.177"
}
```

`dest_sid` must be the Socket.IO session ID of an active station.

---

## Notes

- The test callsign `ZZ0ZZZ` is never broadcast to other clients.
- On initial connection the server sends a `bulk_update` (protocol v2) or individual events (protocol v1) with the current state of all connected stations and recent chat history.
- Clients should key all station state by `sid`, not callsign, since a callsign can reconnect with a new `sid`.
