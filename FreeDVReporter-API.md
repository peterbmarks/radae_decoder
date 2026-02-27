# FreeDV Reporter — JSON API and Window Reference

Note: This documentation was written by Claude Code examining the FreeDV GUI sourcecode. It may not be correct.

## Overview

FreeDV Reporter is an online spotting/activity network for FreeDV operators. The FreeDV GUI connects to a server (default: `qso.freedv.org`, port 80) via **Socket.IO over WebSocket** and exchanges JSON messages to report and receive real-time station activity.

The `FreeDVReporter` class (protocol version **2**) handles all network communication. JSON parsing/serialization uses the embedded **yyjson** library.

---

## Transport

- **Protocol**: Socket.IO (WebSocket upgrade)
- **Default host**: `qso.freedv.org`
- **Default port**: 80
- **Configurable**: hostname and port can be overridden by the user

---

## Connection and Authentication

On connect, the client sends an auth dictionary (Socket.IO auth handshake payload).

### View-only role (no callsign / no grid square configured)

```json
{
  "role": "view",
  "protocol_version": 2
}
```

### Reporting role (callsign and grid square provided)

```json
{
  "role": "report",
  "callsign": "N0CALL",
  "grid_square": "EM00aa",
  "version": "FreeDV GUI v1.x.x",
  "rx_only": false,
  "os": "macOS 14.0",
  "protocol_version": 2
}
```

### Write-only reporting role

Same as the reporting role but with `"role": "report_wo"`. The station reports data but does not receive display updates.

### Role selection logic

| Condition | Role assigned |
|-----------|--------------|
| Callsign or grid square is empty | `"view"` |
| Both set, `writeOnly = true` | `"report_wo"` |
| Both set, `writeOnly = false` | `"report"` |

---

## Client → Server Events

These events are emitted by FreeDV GUI to the server to report local station activity.

### `freq_change` — Frequency update

Sent whenever the operator changes frequency.

```json
{
  "freq": 14236000
}
```

| Field | Type | Description |
|-------|------|-------------|
| `freq` | uint64 | Operating frequency in **Hz** |

---

### `tx_report` — Transmit state change

Sent when PTT is pressed or released.

```json
{
  "mode": "FreeDV 700D",
  "transmitting": true
}
```

| Field | Type | Description |
|-------|------|-------------|
| `mode` | string | FreeDV mode name (e.g. `"FreeDV 700D"`, `"FreeDV 1600"`) |
| `transmitting` | bool | `true` = currently transmitting, `false` = receiving |

---

### `rx_report` — Reception report

Sent immediately when a signal is decoded (no batching).

```json
{
  "callsign": "W1AW",
  "mode": "FreeDV 700D",
  "snr": 12
}
```

| Field | Type | Description |
|-------|------|-------------|
| `callsign` | string | Callsign decoded from the received signal |
| `mode` | string | FreeDV mode in which the signal was decoded |
| `snr` | int | Signal-to-noise ratio in dB |

> **Note:** `rx_report` is only sent after `connection_successful` is received from the server.

---

### `message_update` — User status message

Sent when the operator updates their free-text status message.

```json
{
  "message": "CQ CQ DE W1AW"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `message` | string | Status text (empty string clears the message) |

---

### `qsy_request` — Request another station to QSY

Sent when the operator clicks **Request QSY** for a selected station.

```json
{
  "dest_sid": "abc123def456",
  "frequency": 14236000,
  "message": ""
}
```

| Field | Type | Description |
|-------|------|-------------|
| `dest_sid` | string | Session ID (SID) of the target station |
| `frequency` | uint64 | Frequency in Hz to QSY to |
| `message` | string | Optional human-readable message |

---

### `hide_self` — Hide from display

Emitted (no payload) when the radio enters analog mode. The station is removed from other users' reporter windows.

```
hide_self  (no arguments)
```

---

### `show_self` — Reappear on display

Emitted (no payload) when the radio leaves analog mode. After this, `freq_change`, `tx_report`, and `message_update` are re-sent to restore the station's state.

```
show_self  (no arguments)
```

---

## Server → Client Events

These events arrive from the server and update the FreeDV Reporter window.

### `connection_successful` — Handshake complete

Sent by the server once the client is fully authenticated and registered. No payload fields are inspected. Receiving this event:

1. Sets the client to "fully connected" state
2. Triggers the initial `freq_change`, `tx_report`, and `message_update` transmissions (or `hide_self` if in analog mode)

---

### `new_connection` — A station has connected

```json
{
  "sid": "abc123def456",
  "last_update": "2024-01-15T14:30:00.000Z",
  "callsign": "W1AW",
  "grid_square": "FN31pr",
  "version": "FreeDV GUI v1.9.5",
  "rx_only": false,
  "connect_time": "2024-01-15T14:29:55.000Z"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `sid` | string | Unique session identifier for this connection |
| `last_update` | string | ISO 8601 timestamp of last activity |
| `callsign` | string | Amateur callsign (uppercased by the client) |
| `grid_square` | string | Maidenhead locator (limited to 6 characters by client) |
| `version` | string | Software version string |
| `rx_only` | bool | `true` if the station cannot transmit |
| `connect_time` | string | ISO 8601 timestamp when the session was established |

---

### `remove_connection` — A station has disconnected

```json
{
  "sid": "abc123def456",
  "last_update": "2024-01-15T14:45:00.000Z",
  "callsign": "W1AW",
  "grid_square": "FN31pr",
  "version": "FreeDV GUI v1.9.5",
  "rx_only": false
}
```

| Field | Type | Description |
|-------|------|-------------|
| `sid` | string | Session identifier of the departing station |
| `last_update` | string | ISO 8601 timestamp |
| `callsign` | string | Callsign |
| `grid_square` | string | Grid square |
| `version` | string | Software version |
| `rx_only` | bool | RX-only flag |

> `connect_time` is absent in disconnect messages.

---

### `freq_change` — A station changed frequency

```json
{
  "sid": "abc123def456",
  "last_update": "2024-01-15T14:31:00.000Z",
  "callsign": "W1AW",
  "grid_square": "FN31pr",
  "freq": 14236000
}
```

| Field | Type | Description |
|-------|------|-------------|
| `sid` | string | Session ID of the reporting station |
| `last_update` | string | ISO 8601 timestamp |
| `callsign` | string | Callsign |
| `grid_square` | string | Grid square |
| `freq` | uint64 | New frequency in Hz |

> A frequency change implicitly clears the RX callsign/mode/SNR fields for that station in the UI.

---

### `tx_report` — A station changed TX state

```json
{
  "sid": "abc123def456",
  "last_update": "2024-01-15T14:32:00.000Z",
  "callsign": "W1AW",
  "grid_square": "FN31pr",
  "mode": "FreeDV 700D",
  "transmitting": true,
  "last_tx": "2024-01-15T14:32:00.000Z"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `sid` | string | Session ID |
| `last_update` | string | ISO 8601 timestamp |
| `callsign` | string | Callsign |
| `grid_square` | string | Grid square |
| `mode` | string | FreeDV mode name |
| `transmitting` | bool | `true` = currently transmitting |
| `last_tx` | string \| null | ISO 8601 timestamp of last TX, or `null` if never transmitted |

---

### `rx_report` — A station decoded a signal

```json
{
  "sid": "abc123def456",
  "last_update": "2024-01-15T14:33:00.000Z",
  "receiver_callsign": "W1AW",
  "receiver_grid_square": "FN31pr",
  "callsign": "K1ABC",
  "snr": 8.5,
  "mode": "FreeDV 700D"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `sid` | string | Session ID of the **receiving** station |
| `last_update` | string | ISO 8601 timestamp |
| `receiver_callsign` | string | Callsign of the station that did the receiving |
| `receiver_grid_square` | string | Grid square of the receiving station |
| `callsign` | string | Callsign decoded from the received signal |
| `snr` | number | SNR in dB (may be integer or float) |
| `mode` | string | FreeDV mode used |

> If `callsign` and `mode` are both empty strings, it indicates the station changed frequency and the RX fields should be cleared.

---

### `message_update` — A station updated their status message

```json
{
  "sid": "abc123def456",
  "last_update": "2024-01-15T14:34:00.000Z",
  "message": "CQ CQ listening 14.236 MHz"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `sid` | string | Session ID |
| `last_update` | string | ISO 8601 timestamp |
| `message` | string | Status text (empty string clears it) |

---

### `qsy_request` — Another station requests you QSY

Received when a remote station sends a QSY request targeting the local client.

```json
{
  "callsign": "K1ABC",
  "frequency": 14236000,
  "message": "Please come here"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `callsign` | string | Callsign of the station making the request |
| `frequency` | uint64 | Requested frequency in Hz |
| `message` | string | Optional free-text message |

---

### `bulk_update` — Batched initial state replay

Sent by the server on initial connection to deliver the current state of all connected stations. This is an array of event/data pairs that are replayed in order through the same event handlers as individual events.

```json
[
  ["new_connection", { "sid": "...", "callsign": "W1AW", ... }],
  ["freq_change",    { "sid": "...", "freq": 14236000, ... }],
  ["tx_report",      { "sid": "...", "transmitting": false, ... }],
  ["message_update", { "sid": "...", "message": "CQ", ... }]
]
```

Each element is a two-element array: `[event_name, event_data_object]`. The client fires each event through the normal handler chain.

---

## FreeDV Reporter Window

### Columns

The window is a sortable, resizeable, reorderable data table. Column visibility is user-configurable via the **Show** menu.

| Col # | Header | Description |
|-------|--------|-------------|
| 0 | Callsign | Amateur callsign (always uppercase) |
| 1 | Locator | Maidenhead grid square (up to 6 characters) |
| 2 | km / Miles | Great-circle distance from the local station (Haversine formula) |
| 3 | Hdg / Dir | Bearing to the remote station (degrees or cardinal direction) |
| 4 | Version | Reporter software version string |
| 5 | MHz / kHz | Operating frequency (user-selectable display units) |
| 6 | Mode | Current TX mode (e.g. `FreeDV 700D`) |
| 7 | Status | `TX`, `RX`, or `RX Only` |
| 8 | Msg | Free-text status message (ellipsized; full text shown in tooltip) |
| 9 | Last TX | Timestamp of last transmission |
| 10 | RX Call | Most recently decoded callsign |
| 11 | Mode | Mode used for last decoding (shown only when legacy modes enabled) |
| 12 | SNR | SNR in dB of last decoded signal |
| 13 | Last Update | Timestamp of most recent event from this station |

Default sort order (when no column is selected) is by **connect time** (ascending).

### Row Color Coding

Rows are recolored by a 250 ms timer to reflect recent activity. Colors are theme-aware (light/dark mode).

| Priority | Condition | Meaning |
|----------|-----------|---------|
| 1 (highest) | Message update within **5 s** | Station just sent/updated a status message |
| 2 | Transmitting (`transmitting == true`) | Station is currently on the air |
| 3 | Decoded a valid callsign within **20 s** | Station recently copied another station |
| 4 | Decoded a signal (no callsign) within **5 s** | Station is hearing something but no callsign recovered |
| — | Default | No recent activity |

### Band Filter

The band drop-down filters the station list by frequency band.

| Filter label | Frequency range (Hz) |
|--------------|---------------------|
| All | (no filter) |
| 160 m | 1,800,000 – 2,000,000 |
| 80 m | 3,500,000 – 4,000,000 |
| 60 m | 5,250,000 – 5,450,000 |
| 40 m | 7,000,000 – 7,300,000 |
| 30 m | 10,100,000 – 10,150,000 |
| 20 m | 14,000,000 – 14,350,000 |
| 17 m | 18,068,000 – 18,168,000 |
| 15 m | 21,000,000 – 21,450,000 |
| 12 m | 24,890,000 – 24,990,000 |
| 10 m | 28,000,000 – 29,700,000 |
| >= 6 m | ≥ 50,000,000 |
| Other | Does not fall in any of the above |

**Track** checkbox: When enabled, the filter follows the radio's current frequency automatically (either by band or by exact frequency match).

### Idle Filter

Available via **Filter → Idle more than (minutes)...**: hides stations whose `last_update` is older than a configurable threshold (30, 60, 90, 120 minutes, or custom). The status label in the toolbar shows **Idle N** (highlighted in red) when active, or **Idle Off** when disabled.

### QSY Request

Selecting a station and clicking **Request QSY** sends a `qsy_request` event asking that station to tune to the local operator's current frequency. The button is only enabled when:

- The local station has a valid callsign and grid square configured
- FreeDV is running
- A different station is selected
- The selected station is on a different frequency than the local station

### Status Message

The **Message** text field lets the operator broadcast a free-text status string via `message_update`. A 10-entry MRU (most recently used) history is maintained. Messages can be sent, saved to history, or cleared; the last sent message is persisted across sessions.

### Double-click to QSY

Double-clicking a row in the station list sends the displayed frequency to the connected radio via Hamlib (if frequency control is enabled).

---

## `IReporter` Interface

Both reporting backends implement this abstract interface:

```cpp
class IReporter {
public:
    virtual void freqChange(uint64_t frequency) = 0;
    virtual void transmit(std::string mode, bool tx) = 0;
    virtual void inAnalogMode(bool inAnalog) = 0;
    virtual void addReceiveRecord(std::string callsign, std::string mode,
                                  uint64_t frequency, signed char snr) = 0;
    virtual void send() = 0;
};
```

`FreeDVReporter::send()` is a no-op — it sends data immediately via Socket.IO events rather than batching. `PskReporter::send()` flushes the UDP packet.

---

## Event Sequence — Typical Session

```
Client                              Server
  |                                   |
  |--- connect (auth dictionary) ---->|
  |<-- new_connection (for each       |
  |    existing station) -------------|
  |<-- freq_change / tx_report /      |
  |    message_update (via            |
  |    bulk_update) -----------------|
  |<-- connection_successful ---------|
  |                                   |
  |--- freq_change ------------------>|
  |--- tx_report -------------------->|
  |--- message_update --------------->|
  |                                   |
  |  (during operation)               |
  |--- rx_report -------------------->|   (when a signal is decoded)
  |--- tx_report -------------------->|   (PTT toggled)
  |--- freq_change ------------------>|   (frequency changed)
  |                                   |
  |<-- new_connection ----------------|   (other station joins)
  |<-- freq_change -------------------|   (other station changes freq)
  |<-- tx_report ---------------------|   (other station goes TX)
  |<-- rx_report ---------------------|   (other station copies someone)
  |<-- message_update ----------------|   (other station updates message)
  |<-- remove_connection -------------|   (other station leaves)
  |                                   |
  |--- hide_self -------------------->|   (entering analog mode)
  |--- show_self -------------------->|   (leaving analog mode)
  |--- freq_change ------------------>|   (state re-sent after show_self)
  |--- tx_report -------------------->|
  |--- message_update --------------->|
```

# Example
```
15:12:10 [18] INFO /home/marksp/freedv-rade/freedv-gui/src/util/TcpConnectionHandler.cpp:163: Attempting connection to qso.freedv.org port 80
15:12:10 [19] INFO /home/marksp/freedv-rade/freedv-gui/src/util/TcpConnectionHandler.cpp:203: waiting for DNS
15:12:10 [20] INFO /home/marksp/freedv-rade/freedv-gui/src/util/TcpConnectionHandler.cpp:787: attempting to resolve qso.freedv.org:80 using family 2
15:12:10 [21] INFO /home/marksp/freedv-rade/freedv-gui/src/util/TcpConnectionHandler.cpp:787: attempting to resolve qso.freedv.org:80 using family 10
15:12:10 [22] INFO /home/marksp/freedv-rade/freedv-gui/src/util/TcpConnectionHandler.cpp:804: resolution of qso.freedv.org:80 using family 10 complete
15:12:10 [23] INFO /home/marksp/freedv-rade/freedv-gui/src/util/TcpConnectionHandler.cpp:210: some DNS results are available (v4 = 0, v6 = 1)
15:12:10 [24] INFO /home/marksp/freedv-rade/freedv-gui/src/util/TcpConnectionHandler.cpp:248: starting with IPv6 connection
15:12:10 [25] INFO /home/marksp/freedv-rade/freedv-gui/src/util/TcpConnectionHandler.cpp:324: attempting connection to 2602:fffa:fff:108a:216:3eff:fe32:ba39
15:12:10 [26] INFO /home/marksp/freedv-rade/freedv-gui/src/util/TcpConnectionHandler.cpp:804: resolution of qso.freedv.org:80 using family 2 complete
15:12:10 [27] INFO /home/marksp/freedv-rade/freedv-gui/src/util/TcpConnectionHandler.cpp:473: connection succeeded to 2602:fffa:fff:108a:216:3eff:fe32:ba39
15:12:10 [28] INFO /home/marksp/freedv-rade/freedv-gui/src/util/ulog_logger.h:138: [connect] Successful connection
15:12:10 [29] INFO /home/marksp/freedv-rade/freedv-gui/src/util/ulog_logger.h:127: [connect] WebSocket Connection iostream transport v-2 "WebSocket++/0.8.2" /socket.io/?EIO=4&transport=websocket 101
received: 0{"sid":"6bDgMcNPpp_CaqrjDW2W","upgrades":[],"pingTimeout":5000,"pingInterval":5000,"maxPayload":1000000}
15:12:10 [30] INFO /home/marksp/freedv-rade/freedv-gui/src/util/SocketIoClient.cpp:213: engine.io open
received: 40{"sid":"IDE0MRPIIpPbh9juDW2X"}
15:12:11 [31] INFO /home/marksp/freedv-rade/freedv-gui/src/util/SocketIoClient.cpp:282: socket.io connect
received: 42["new_connection",{"sid":"IDE0MRPIIpPbh9juDW2X","last_update":"2026-02-27T04:12:11.156127+00:00","callsign":"VK3TPM","grid_square":"QF22ds","version":"FreeDV 2.3.0-dev-1e14","rx_only":false,"os":"linux","connect_time":"2026-02-27T04:12:11.156111+00:00"}]
received: 42["bulk_update",[["new_connection",{"sid":"JwL2YYl-ky3GKb_nDLM-","last_update":"2026-02-26T06:50:06.552962+00:00","callsign":"KJ0CFW","grid_square":"DM78pr","version":"FreeDV 2.2.1","rx_only":false,"os":"windows","connect_time":"2026-02-24T18:36:32.714673+00:00"}],["freq_change",{"sid":"JwL2YYl-ky3GKb_nDLM-","last_update":"2026-02-26T06:50:06.552962+00:00","callsign":"KJ0CFW","grid_square":"DM78pr","freq":14236000}],["tx_report",{"sid":"JwL2YYl-ky3GKb_nDLM-","last_update":"2026-02-26T06:50:06.552962+00:00","callsign":"KJ0CFW","grid_square":"DM78pr","mode":"RADEV1","transmitting":false,"last_tx":"2026-02-24T18:37:26.876509+00:00"}],["message_update",{"sid":"JwL2YYl-ky3GKb_nDLM-","last_update":"2026-02-26T06:50:06.552962+00:00","message":""}],["new_connection",{"sid":"8N1AhX1g2g0qvmjCDLrg","last_update":"2026-02-24T20:26:37.658903+00:00","callsign":"N2OA","grid_square":"FN03va","version":"freedv-flex 2.1.0","rx_only":false,"os":"linux","connect_time":"2026-02-24T20:26:37.299867+00:00"}],["freq_change",{"sid":"8N1AhX1g2g0qvmjCDLrg","last_update":"2026-02-24T20:26:37.658903+00:00","callsign":"N2OA","grid_square":"FN03va","freq":3625000}],["tx_report",{"sid":"8N1AhX1g2g0qvmjCDLrg","last_update":"2026-02-24T20:26:37.658903+00:00","callsign":"N2OA","grid_square":"FN03va","mode":"RADEV1","transmitting":false,"last_tx":null}],["message_update",{"sid":"8N1AhX1g2g0qvmjCDLrg","last_update":"2026-02-24T20:26:37.658903+00:00","message":""}],["new_connection",{"sid":"5vQuVqI-erWvmbaFDMhD","last_update":"2026-02-24T23:49:55.272227+00:00","callsign":"ND3I","grid_square":"FN20ka","version":"FreeDV 2.0.1","rx_only":true,"os":"windows","connect_time":"2026-02-24T23:49:54.786415+00:00"}],["freq_change",{"sid":"5vQuVqI-erWvmbaFDMhD","last_update":"2026-02-24T23:49:55.272227+00:00","callsign":"ND3I","grid_square":"FN20ka","freq":7177000}],["tx_report",{"sid":"5vQuVqI-erWvmbaFDMhD","last_update":"2026-02-24T23:49:55.272227+00:00","callsign":"ND3I","grid_square":"FN20ka","mode":"RADEV1","transmitting":false,"last_tx":null}],["message_update",{"sid":"5vQuVqI-erWvmbaFDMhD","last_update":"2026-02-24T23:49:55.272227+00:00","message":""}],["new_connection",{"sid":"jgQ-sEbzvPfwAERfDNeb","last_update":"2026-02-26T14:28:05.471347+00:00","callsign":"N9RIX","grid_square":"EN50xk","version":"FreeDV 2.2.1","rx_only":false,"os":"windows","connect_time":"2026-02-25T06:06:54.135088+00:00"}],["freq_change",{"sid":"jgQ-sEbzvPfwAERfDNeb","last_update":"2026-02-26T14:28:05.471347+00:00","callsign":"N9RIX","grid_square":"EN50xk","freq":24955700}],["tx_report",{"sid":"jgQ-sEbzvPfwAERfDNeb","last_update":"2026-02-26T14:28:05.471347+00:00","callsign":"N9RIX","grid_square":"EN50xk","mode":"RADEV1","transmitting":false,"last_tx":"2026-02-25T21:58:16.385504+00:00"}],["message_update",{"sid":"jgQ-sEbzvPfwAERfDNeb","last_update":"2026-02-26T14:28:05.471347+00:00","message":"    Rob - Paxton, IL."}],["new_connection",{"sid":"2xdsZIAKTdbKUtgDDQ30","last_update":"2026-02-27T04:07:20.114142+00:00","callsign":"JI3LDJ","grid_square":"PM74vw","version":"FreeDV 2.2.1","rx_only":true,"os":"windows","connect_time":"2026-02-26T00:18:54.039637+00:00"}],["freq_change",{"sid":"2xdsZIAKTdbKUtgDDQ30","last_update":"2026-02-27T04:07:20.114142+00:00","callsign":"JI3LDJ","grid_square":"PM74vw","freq":0}],["tx_report",{"sid":"2xdsZIAKTdbKUtgDDQ30","last_update":"2026-02-27T04:07:20.114142+00:00","callsign":"JI3LDJ","grid_square":"PM74vw","mode":"RADEV1","transmitting":false,"last_tx":null}],["message_update",{"sid":"2xdsZIAKTdbKUtgDDQ30","last_update":"2026-02-27T04:07:20.114142+00:00","message":"F\u5c64\u81e8\u754c\u5468\u6ce2\u6570(\u6771\u4eac) 12.3[MHz] - 13:00 (02/27) \uff0f\u5317\u6d77\u9053\u

```