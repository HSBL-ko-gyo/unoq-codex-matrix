# Protocol specification

## Compatibility

Protocol version: **1**

The Python daemon and STM32 firmware must use the same version and numeric state IDs. Unknown versions, event names, fields, state IDs, malformed JSON/MessagePack, oversized messages, and out-of-range arguments are rejected without terminating the daemon or firmware loop.

## State IDs

These values are a public wire ABI. Never reorder or reuse them.

| ID | Symbol | Wire name |
|---:|---|---|
| 0 | `OFF` | `off` |
| 1 | `IDLE` | `idle` |
| 2 | `THINKING` | `thinking` |
| 3 | `READING` | `reading` |
| 4 | `WRITING` | `writing` |
| 5 | `COMMAND` | `command` |
| 6 | `BUILDING` | `building` |
| 7 | `TESTING` | `testing` |
| 8 | `FLASHING` | `flashing` |
| 9 | `WAITING` | `waiting` |
| 10 | `SUCCESS` | `success` |
| 11 | `ERROR` | `error` |
| 12 | `OFFLINE` | `offline` |
| 13 | `SUBAGENT` | `subagent` |

This table is the normative documentation. The consistency test compares the
expected IDs with `State` in `src/unoq_codex_matrix/protocol.py` and `StateId` in
`firmware/unoq_codex_matrix/protocol.h`; documentation review must keep this
table aligned as part of a protocol change.

## Codex Hook input

[Codex Hooks](https://learn.chatgpt.com/docs/hooks) send one JSON object on stdin. The observer recognizes these events:

| Codex event | Normalized event | Default state |
|---|---|---|
| `SessionStart` | `session_start` | IDLE |
| `UserPromptSubmit` | `user_prompt` | THINKING |
| `PreToolUse` | `pre_tool` | Classified from tool/input |
| `PermissionRequest` | `permission` | WAITING |
| `PostToolUse` | `post_tool` | ERROR only for explicit failure, otherwise THINKING |
| `PreCompact` | `pre_compact` | THINKING |
| `PostCompact` | `post_compact` | THINKING |
| `SubagentStart` | `subagent_start` | SUBAGENT |
| `SubagentStop` | `subagent_stop` | THINKING |
| `Stop` | `stop` | SUCCESS |
| `SessionEnd` | `session_end` | Removes session |

The raw stdin guard is 65,536 bytes. Oversized or malformed input is dropped. Raw Hook input is never logged.

### Tool classification

`PreToolUse` maps edit/write tools to WRITING; read/grep/glob/search tools to
READING; and agent/spawn tools to SUBAGENT. The primary native Hook uses a
bounded quote-aware tokenizer; the Python reference uses `shlex` with equivalent
classification tests. Neither attempts to implement a complete shell parser.
Malformed or ambiguous input safely becomes COMMAND.

Recognized FLASHING commands include:

```text
arduino-cli upload
remoteocd
openocd
dfu-util
west flash
bossac
```

Recognized TESTING commands include common `pytest`, `python -m pytest`, npm/pnpm/yarn test, Cargo/Go tests, CTest, Meson test, Make test, Jest, and Vitest forms. Recognized BUILDING commands include `arduino-cli compile`, `cmake --build`, Ninja, Make, Cargo build, npm/pnpm/yarn build, and Vite build forms. For a compound command, priority is FLASHING, then TESTING, then BUILDING; otherwise COMMAND.

### Explicit failure

PostToolUse sets `failed: true` only for structured data such as:

- non-zero `exit_code` or `return_code`;
- `status` equal to `failed`, `failure`, or `error`;
- `success: false`;
- `is_error: true` or `isError: true`;
- a non-empty explicit `error`, `tool_error`, or `toolError` field;
- the same fields in a dedicated `metadata` mapping.

Free-form response text is never searched for words such as “error”.

## Hook-to-daemon datagram

Socket: `/run/unoq-codex-matrix/events.sock`

Type: `AF_UNIX`, `SOCK_DGRAM`, non-blocking sender

Maximum datagram: 4,096 bytes

Encoding: compact UTF-8 JSON object with no newline requirement

Example with non-sensitive placeholder identifiers:

```json
{"v":1,"event":"pre_tool","session":"session-example","turn":"turn-example","tool_use":"item-example","tool":"Bash","state":"testing","failed":false,"time_ns":123456789}
```

All fields are emitted, including empty optional strings.

| Field | Type | Limit/meaning |
|---|---|---|
| `v` | integer | Must equal 1 |
| `event` | string | One normalized event from the table above |
| `session` | string | Required, non-empty, maximum 256 characters |
| `turn` | string | Optional, maximum 256 characters |
| `tool_use` | string | Optional, maximum 256 characters |
| `tool` | string | Optional, maximum 128 characters; empty outside tool events |
| `state` | string or integer | Canonical state name is emitted; decoder also accepts fixed ID |
| `failed` | boolean | Explicit failure only |
| `time_ns` | non-negative integer | Hook host monotonic nanoseconds |

No other field is permitted. The daemon rejects datagrams with unknown keys. In particular, there is no field for prompt, message, reasoning, command, tool input/output, path, environment, diff, transcript, or credential data.

The daemon accepts timestamps no more than five seconds into its future and no older than `stale_session_s`. Duplicate identity is exactly:

```text
(session, turn, tool_use, event)
```

An older non-duplicate event cannot roll a session back past its last monotonic timestamp.

## Codex quota source

When `show_quota_bar` is enabled, the daemon starts `codex app-server` with its
default JSONL stdio transport, completes the initialization handshake, and
periodically sends the
[official read-only rate-limit request](https://github.com/openai/codex/blob/main/codex-rs/app-server/README.md#7-rate-limits-chatgpt):

```json
{"method":"account/rateLimits/read","id":2}
```

Only `usedPercent` from the `codex` bucket's `primary` and `secondary` windows
is retained. Remaining percentage is `100 - usedPercent`; when both windows
exist, the lower remaining value wins. Window names, account identifiers,
plan, credit balance, reset timestamps, notifications, and error bodies are not
retained or logged. A missing, malformed, failed, or stale response hides the
quota bar without affecting lifecycle state observation.

## Daemon aggregation semantics

The global winner uses this fixed priority, with the newest session update as
the tie-breaker:

```text
ERROR > WAITING > FLASHING > TESTING > BUILDING > WRITING >
READING > COMMAND > SUBAGENT > THINKING > SUCCESS > IDLE > OFF
```

OFFLINE has the highest numeric rank when explicitly represented, although MCU
heartbeat timeout is the normal way a transport outage becomes visible.
SUCCESS changes to IDLE after `success_hold_s` (8 seconds by default). A
PostToolUse ERROR changes to THINKING after `transient_error_s` (1.5 seconds by
default). `SessionEnd` removes the active record, while an eight-second
content-free completion lease preserves a preceding SUCCESS until its original
expiry. A bounded SessionEnd tombstone prevents delayed events from resurrecting
that session. Silent records, tombstones, and deduplication metadata expire by
`stale_session_s` (12 hours by default) or bounded capacity. Duplicate keys are
stored in an 8,192-entry in-memory cache. None of this metadata is logged or
persisted.

## CLI control protocol

Socket: `/run/unoq-codex-matrix/control.sock`

Type: `AF_UNIX`, `SOCK_STREAM`

Framing: one ASCII JSON object followed by `\n`; one response followed by connection close

Maximum object size: 8,192 bytes

Requests:

```json
{"action":"status"}
{"action":"doctor"}
{"action":"demo"}
{"action":"set","state":"testing","duration_s":10}
```

`duration_s` must be 0.1 through 3,600. `demo` walks `OFF` through `SUBAGENT`, then `IDLE`, with a two-second step. The response reports its total duration. Errors use:

```json
{"ok":false,"error":"bounded non-sensitive message"}
```

Status fields are:

```text
ok
daemon_status
router_status
mcu_protocol_version
current_state
active_session_count
last_hook_event_age_s
last_mcu_heartbeat_age_s
brightness
firmware_version
codex_quota_remaining_percent
codex_quota_source_status
firmware_quota_bar_supported
```

`doctor` adds `checks` and `healthy`. No session identifier is returned.

`active_session_count` counts tracked records, including IDLE/SUCCESS records
that have not received `SessionEnd` or expired. `last_mcu_heartbeat_age_s` is the
age of the daemon's most recent successful MCU RPC response; it is not a
separately persisted heartbeat history. Acceptance of a `set` or `demo` control
request schedules a daemon override but does not by itself prove the current MCU
publish succeeded.

`codex_quota_source_status` is `disabled`, `starting`, `available`,
`unavailable`, or `stale`. The remaining percentage is null unless a current
snapshot exists. Quota-source health is reported by `doctor`, but it is an
optional overlay and does not make an otherwise healthy lifecycle/Router/MCU
path fail the command.

## Arduino Router MessagePack RPC

Socket: `/var/run/arduino-router.sock`

The wire format follows the official [Arduino Router MessagePack RPC description](https://github.com/arduino/arduino-router/blob/main/msgpackrpc/README.md).

Request:

```text
[0, message_id, method, params]
```

Response:

```text
[1, message_id, error, result]
```

The Linux client uses a 4,096-byte unpack buffer, a 250 ms connect timeout, a 500 ms response timeout, monotonically increasing non-zero 32-bit message IDs, and exactly one locked in-flight request per connection. A transport failure, timeout, MessagePack decoding failure, oversized request, or non-empty RPC error closes the connection so the next daemon attempt reconnects. A well-framed but rejected, out-of-range, or protocol-mismatched result raises a daemon-visible error; the persistent socket is not guaranteed to close in that case.

### MCU methods

#### `codex_matrix_set_state`

Parameters:

```text
[
  protocol_version,       # 1
  state_id,               # 0..13
  active_sessions,        # 0..255
  frame_interval_ms,      # 50..150
  offline_timeout_ms,     # firmware accepts 1000..600000
  show_active_count       # 0 or 1
]
```

Result: `1` when accepted, `0` when rejected. A valid state publish also refreshes the MCU heartbeat. A correctly versioned request with an invalid state ID stages ERROR as a visible protocol fault and refreshes the heartbeat; the next valid publish recovers it. Other invalid arguments are rejected without replacing the last requested state.

The published frame interval remains validated as 50..150 ms for protocol
compatibility. THINKING internally caps its effective render cadence at 16 ms,
matching Arduino's native boot-animation playback;
IDLE uses 32 ms only for its 210 ms entry fade and then resends its static frame
every 750 ms. Other states retain the published interval, and the 4.2-second
THINKING lap speed is unchanged.

#### `codex_matrix_heartbeat`

Parameters: `[protocol_version]`

Result: `1` when accepted, `0` when rejected.

#### `codex_matrix_set_brightness`

Parameters: `[protocol_version, level]`

Result for protocol v1: effective brightness. Firmware clamps it to project range 0..5; the physical 3-bit matrix range is 0..7. A protocol mismatch leaves brightness unchanged; the subsequent heartbeat/version checks make the overall publish fail.

#### `codex_matrix_set_quota`

Firmware 0.2.0 and later exposes this additive protocol-v1 method. Parameters:

```text
[
  protocol_version,       # 1
  remaining_percent,      # 0..100
  visible                  # 0 or 1
]
```

Result: `1` when accepted, `0` when rejected. The daemon reads firmware version
before publishing and does not call this method on older firmware, preserving
the pre-quota state path.

When visible, the bottom matrix row is reserved for a left-to-right 13-segment
bar. Zero percent lights no segment; every non-zero value lights at least one;
100 percent lights all 13. The renderer uses ceiling division, so one segment
represents approximately 7.7 percentage points. OFF and OFFLINE omit the bar.
The upper-right active-session dots are independent and can be shown at the same
time. If quota is unavailable or stale, the daemon hides only the quota bar.

#### `codex_matrix_get_status`

Parameters: `[]`

Result: unsigned 32-bit packed integer:

```text
bits 31..24  protocol version
bits 23..16  currently displayed state ID
bits 15..8   active session count
bits 7..0    effective brightness
```

#### `codex_matrix_get_version`

Parameters: `[]`

Result: unsigned 32-bit packed integer:

```text
bits 31..24  protocol version
bits 23..16  firmware major
bits 15..8   firmware minor
bits 7..0    firmware patch
```

Firmware `0.1.0` is the initial state-only release; the quota-capable firmware
is `0.2.0`.

#### `codex_matrix_get_render_metrics`

Parameters: `[]`

Result: unsigned 32-bit diagnostic value. Bits 31..16 contain the average
renderer-plus-matrix-draw duration in microseconds and bits 15..0 contain the
maximum duration in the current bounded sample window. This additive
diagnostic RPC does not change protocol version 1 or any state ID.

## Heartbeat and OFFLINE behavior

- Firmware boots into OFFLINE.
- A valid full state publish or heartbeat marks Linux online.
- The daemon republishes at the configured heartbeat interval, 3 seconds by default.
- If unsigned `millis()` elapsed time exceeds the published offline timeout, 12 seconds by default, firmware renders OFFLINE.
- After Router/daemon recovery, a complete publish restores state, count,
  timing, brightness, supported quota overlay, and heartbeat.

## Versioning rules

- Additive daemon-control status fields may be introduced without changing protocol v1 if old clients can ignore them.
- Additive MCU RPC methods may be introduced without changing protocol v1 when
  the daemon detects firmware support before calling them and all existing
  methods retain their parameters and meaning.
- Any change to state IDs, datagram field meaning, RPC parameter order, or packed word layout requires a new protocol version.
- A new state must be added to both Python and C++ with a consistency test before release.
- Raw Hook schema changes do not alter this wire protocol unless the normalized allow-list changes.
