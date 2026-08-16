# UNO Q Codex Matrix

Show live OpenAI Codex activity on the Arduino UNO Q onboard 8x13 blue LED matrix.

[日本語](README.ja.md) | [Architecture](docs/architecture.md) | [Protocol](docs/protocol.md) | [Privacy](docs/privacy.md)

> [!IMPORTANT]
> This repository is currently **v0.1.0-alpha.1**. On a real UNO Q, Codex 0.147.0 lifecycle events, the local Hook/daemon pipeline, Router RPC, firmware compile/upload, the state demo, TTLs, OFFLINE recovery, and a 30-minute hardware soak have been exercised. Phone-originated Remote Hooks, a privileged systemd installation, and visual confirmation of every physical animation remain open. See [investigation results](docs/investigation.md) before treating this as a stable release.

UNO Q Codex Matrix is an independent observation layer. Codex does not need an instruction, prompt, or `AGENTS.md` entry telling it to update the display. A short-lived lifecycle Hook emits a privacy-reduced local event, a daemon aggregates all active sessions, and the STM32 firmware renders the selected animation.

## Demo

<!-- Replace this block with a photo or short looping demo after visual validation. -->

> The all-state RPC demo has run on flashed hardware. Demo media and a human visual-distinction review are still pending.

Verified flow: a prompt enters THINKING, a recognized read tool enters READING, `apply_patch` enters WRITING, a recognized test command enters TESTING, and `Stop` holds SUCCESS before returning to IDLE. Codex 0.147.0 sometimes performs reads through Bash; those events intentionally remain COMMAND because the Hook does not infer shell intent beyond the documented command categories.

## Architecture

```mermaid
flowchart LR
    Phone["ChatGPT on a phone"] -->|"Remote"| Codex["Codex on UNO Q Linux"]
    Codex -->|"Lifecycle Hooks"| Hook["unoq-codex-matrix-hook"]
    Hook -->|"Unix datagram<br/>events.sock"| Daemon["unoq-codex-matrixd"]
    Daemon --> Aggregate["Multi-session aggregator"]
    Aggregate -->|"MessagePack RPC"| Router["Arduino Router"]
    Router --> MCU["STM32U585 firmware"]
    MCU --> Matrix["8x13 blue LED matrix"]
```

The Hook never waits for a reply, retries, or opens a network connection. The daemon owns aggregation, TTLs, Router reconnection, heartbeat, and the local CLI control socket. Firmware RPC callbacks only stage bounded values; animation and matrix drawing happen in `loop()`.

The installed primary Hook is a dependency-free native C executable. The
installer builds it with an existing C compiler, or privately downloads and
extracts Debian TCC/libc build files into a temporary directory without
installing compiler packages. A `jq`/`socat` shell implementation remains a
portable fallback only when the native build cannot complete.

The lifecycle-Hook path is the only v0.1 event input. `src/unoq_codex_matrix/sources.py` exposes the `EventSource` protocol and its concrete `CodexHooksSource`; the daemon consumes that boundary without coupling aggregation to the transport. App Server and Codex Micro HID inputs are future, optional experiments and are not fallback parsers in this release.

## Supported hardware and software

- Arduino UNO Q with the STM32U585-controlled onboard 8x13 matrix.
- UNO Q Linux running systemd and `arduino-router.service`.
- Arduino CLI with FQBN `arduino:zephyr:unoq`.
- Arduino UNO Q Zephyr core; the investigated board had `arduino:zephyr` 0.90.0.
- `Arduino_LED_Matrix`, plus `Arduino_RouterBridge` pinned to 0.4.3.
- Python 3.11 or later. The investigated board had Python 3.13.5.
- A Codex build that supports user lifecycle Hooks. Real-device probing used the official Linux ARM64 Codex 0.147.0 release.

The system Python does not need a global `msgpack` install. The installer creates a private virtual environment and installs the declared Python dependency there.

## Real-device validation status

The alpha hardware pass used Debian 13 on AArch64, Arduino Router 0.9.0, `arduino:zephyr` 0.90.0, and `Arduino_RouterBridge` 0.4.3.

- Observed Hooks: SessionStart, UserPromptSubmit, PreToolUse, PermissionRequest, PostToolUse, SubagentStart, SubagentStop, Stop, and SessionEnd. PreCompact and PostCompact were not forced.
- Compiled and uploaded the STM32U585 firmware through `arduino-cli`; protocol version 1 and firmware version 0.1.0 were returned over Router RPC.
- Exercised all state IDs, brightness clamping, 1,000 rapid state changes, SUCCESS and transient-ERROR TTLs, daemon restart, heartbeat timeout to OFFLINE, and recovery to the current state.
- Ran a dedicated local Codex smoke task without any LED instruction and observed Hook-driven state changes, including a real permission request and subagent lifecycle.
- Qualified the final native Hook against the live daemon under the board's existing high CPU load: 200 measured runs after 20 warmups had median 3.285 ms, p95 3.573 ms, and maximum 3.746 ms. Absent-socket and dummy-receiver p95 values were 3.513 ms and 3.543 ms.
- Passed 47 native integration tests under GCC 14 with `-Werror`, AddressSanitizer, and UndefinedBehaviorSanitizer, plus 300 malformed random inputs and 500 valid differential cases against the Python reference.
- Completed a 1,801.2-second continuous hardware animation/RPC soak with no RPC failure, sampling THINKING, TESTING, OFFLINE, WRITING, WAITING, and IDLE during the run.
- Did not install the system service on the investigated board because non-interactive administrative access was unavailable. The daemon and control sockets were exercised manually without altering the board's privilege configuration.
- Phone-originated Remote behavior and human confirmation that every animation is visually distinct remain unverified.

Codex 0.147.0 supplied Bash and `apply_patch` `tool_response` values as strings in the observed Hooks. The project never searches response text for failure words, so a real failing Bash command did not produce ERROR on that build. Structured synthetic failure fields do produce the specified transient ERROR. This privacy-preserving compatibility limitation is why the release remains alpha.

## Five-minute installation overview

Run these commands on the UNO Q:

```bash
git clone https://github.com/HSBL-ko-gyo/unoq-codex-matrix.git
cd unoq-codex-matrix
sudo ./scripts/install.sh
```

The idempotent installer creates `/opt/unoq-codex-matrix/venv`, installs three commands under `/usr/local/bin`, writes the initial config only when absent, installs and starts the systemd service, merges user Hooks without replacing existing handlers, compiles and uploads the firmware, then runs the demo and doctor.

Useful install variants:

```bash
sudo ./scripts/install.sh --no-flash
sudo ./scripts/install.sh --no-hooks
sudo ./scripts/install.sh --no-start
```

`--no-flash` is useful for reviewing the Linux side first. The installer never upgrades the OS or changes the Router, network, SSH, desktop, kernel, bootloader, or global Git configuration.

## Review and trust the Hook

[Codex Hooks](https://learn.chatgpt.com/docs/hooks) require non-managed command definitions to be reviewed and trusted by their current hash. In Codex running as the `arduino` user:

1. Open `/hooks`.
2. Locate the user-level entries added for `/usr/local/bin/unoq-codex-matrix-hook`.
3. Review the exact command and all lifecycle events.
4. Trust the definition, then start a new ordinary task.

Do not make `--dangerously-bypass-hook-trust` a persistent setting. If `[features].hooks = false` is present in the active Codex config, enable Hooks deliberately and review them again. The installer uses `~/.codex/hooks.json` unless that same user layer already uses inline `[hooks]` tables; it will not silently mix both forms.

## States and animations

State IDs are a versioned wire ABI shared by Python and C++.

| ID | State | Typical trigger | Matrix animation |
|---:|---|---|---|
| 0 | OFF | CLI override | Completely dark |
| 1 | IDLE | Session start or success expiry | Static level-1 three-dot READY indicator centered at x=4, 6, 8 after a 210 ms entry fade |
| 2 | THINKING | Prompt submitted, tool completed, compaction | 3-bit grayscale, Q8-interpolated comet and decaying trail flow around a balanced inner infinity path at 16 ms/frame; bubbles rise on independent subpixel paths with individual fades, while the whole state fades in over 420 ms |
| 3 | READING | Read, grep, glob, or search tool | Vertical scanning line |
| 4 | WRITING | `apply_patch`, Edit, or Write | Serpentine writing cursor and trail |
| 5 | COMMAND | Other shell/tool activity | Right-moving arrow pulse |
| 6 | BUILDING | Recognized build command | Blocks stack from the bottom |
| 7 | TESTING | Recognized test command | Bidirectional progress bar |
| 8 | FLASHING | Firmware upload command | Falling data streams |
| 9 | WAITING | Permission request | Slowly blinking question mark |
| 10 | SUCCESS | `Stop` | Check mark flashes twice, then holds |
| 11 | ERROR | Explicit tool failure | X flashes, then remains dim |
| 12 | OFFLINE | MCU heartbeat timeout | Slow disconnected/exclamation glyph |
| 13 | SUBAGENT | Subagent start | Three independently moving dots |

Up to three dim dots in the top-right corner show the number of tracked, not-yet-ended sessions (including a tracked IDLE or SUCCESS session). OFF and OFFLINE omit those dots. SUCCESS is held for 8 seconds by default; a tool ERROR is transient for 1.5 seconds. Active work in another session outranks SUCCESS.

## CLI

```bash
unoq-codex-matrix status
unoq-codex-matrix demo
unoq-codex-matrix set thinking
unoq-codex-matrix set testing --seconds 15
unoq-codex-matrix set waiting
unoq-codex-matrix set success
unoq-codex-matrix set error
unoq-codex-matrix off
unoq-codex-matrix doctor
unoq-codex-matrix version
```

`status` reports daemon, Router, MCU protocol, global state, tracked-session count, Hook and MCU-RPC ages, brightness, and firmware version. It does not print session IDs, prompts, or commands. `demo` displays every state for about two seconds, uses a final IDLE handoff, and then returns to the aggregated session state. `off` is a 30-second override rather than a permanent power setting. A successful `set`/`demo` reply means the daemon accepted the control request; inspect `status` or `doctor` plus the reported MCU-RPC age when checking connectivity.

## Configuration

Runtime configuration lives at `/etc/unoq-codex-matrix/config.json`:

```json
{
  "brightness": 3,
  "frame_interval_ms": 100,
  "heartbeat_interval_s": 3,
  "offline_timeout_s": 12,
  "success_hold_s": 8,
  "transient_error_s": 1.5,
  "stale_session_s": 43200,
  "show_active_count": true,
  "log_level": "INFO"
}
```

Values are range-checked. A missing, malformed, or out-of-range value falls back to a safe default and produces a small journal warning. Restart the daemon after editing:

```bash
sudo systemctl restart unoq-codex-matrix.service
```

## Privacy

The observer uses only:

- lifecycle event name;
- coarse tool category/name;
- session, turn, and tool-use identifiers held in daemon memory;
- explicit structured success/failure information;
- monotonic timestamps.

It does **not** store or transmit prompts, answers, reasoning, command text, file contents, diffs, transcripts, credentials, cookies, or environment variables. A Bash command is inspected in Hook memory only to choose COMMAND, BUILDING, TESTING, or FLASHING, then discarded. Normal logs omit session IDs. No external HTTP endpoint, MQTT connection, or network telemetry exists. See [the full privacy design](docs/privacy.md).

## Troubleshooting

Start with:

```bash
unoq-codex-matrix doctor
systemctl status unoq-codex-matrix.service --no-pager
systemctl status arduino-router.service --no-pager
journalctl -u unoq-codex-matrix.service -n 100 --no-pager
```

`doctor` checks the two project sockets, the Router socket, MCU RPC, and protocol
match. It does not prove Hook trust, Codex feature configuration, systemd unit
contents, or firmware-upload history; review those separately when relevant.

If the Hook does not run, inspect `/hooks`, confirm the exact command is trusted, and check that Hooks are not disabled in the active config. If the daemon says `reconnecting`, verify the Router socket exists; do not open `/dev/ttyHS1` or use MCU `Serial1` directly. If the matrix shows OFFLINE, check daemon/Router health and firmware protocol version. If Remote-originated work produces no Hook event, follow [the Remote test](docs/remote-test.md); do not enable session-JSONL parsing as an automatic fallback.

ERROR requires an explicit structured failure field. A Codex build that exposes only a string `tool_response` can show TESTING followed by THINKING for a failed test, without the transient ERROR; this is deliberate rather than a response-body heuristic.

## Uninstall

Remove the service, installed commands, virtual environment, and only this project's Hook handlers:

```bash
sudo ./scripts/uninstall.sh
```

Keep the config by default, or remove it explicitly:

```bash
sudo ./scripts/uninstall.sh --purge
```

The uninstaller does not remove packages, logs, or existing Hooks and does not replace the MCU firmware with another sketch. See [rollback](docs/rollback.md) for backup restoration and firmware recovery.

## Development

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -e '.[dev]'
pytest
python -m compileall -q src tests
./scripts/build-native-hook.sh
arduino-cli compile -b arduino:zephyr:unoq firmware/unoq_codex_matrix
```

Hardware tests are intentionally separate from CI. Uploading changes the STM32 application, so use `./scripts/flash-firmware.sh` only on the intended UNO Q after compile succeeds. Contributions should preserve the fail-open Hook, privacy allow-list, Python/C++ state-ID consistency, and independent implementation. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Prior art

The design was informed by six community projects covering Hook/daemon separation and Codex Micro status devices. No source code was copied. GPL-licensed and unlicensed implementations were used only as behavioral references. See [prior art](docs/prior-art.md) and [third-party notices](THIRD_PARTY.md).

## Roadmap

- Verify lifecycle Hooks for phone-originated Remote turns.
- Complete the human visual-distinction review.
- Perform the privileged idempotent install/uninstall and system-boot service test.
- Publish reproducible hardware-test evidence and demo media.
- Consider an `AppServerSource` only after non-disruptive observation is demonstrated.
- Keep `CodexMicroHidSource` experimental and out of the default build.

Not planned for v0.1: Vendor HID emulation, borrowed VID/PID values, a web dashboard, public HTTP, MQTT, prompt/transcript logging, or automatic commits and pull requests.

## License and trademarks

MIT. This is an unofficial community project and is not affiliated with or endorsed by OpenAI, Codex, or Arduino. Product names and trademarks belong to their respective owners.
