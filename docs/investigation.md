# UNO Q investigation

## Status and scope

These sanitized observations were collected on a real Arduino UNO Q on
2026-08-16. Hostnames, addresses, user-specific paths, identifiers, Hook
payloads, commands, transcripts, and authentication data are intentionally
omitted. The Codex authentication file was never opened, displayed, or copied.

The record distinguishes the pre-change baseline from later alpha validation.
Codex was absent at the first check; the official Linux ARM64 Codex 0.147.0
release was then installed in a user-local executable directory and used for
the lifecycle probe and disposable smoke task. No OS-wide upgrade was run.

## Baseline command results

| Command | Sanitized result |
|---|---|
| `uname -a` | Linux kernel 7.0.0 on AArch64; hostname/build details omitted |
| `uname -m` | `aarch64` |
| `cat /etc/os-release` | Debian GNU/Linux 13 (`trixie`) |
| `free -h` | Approximately 3.6 GiB total RAM |
| `df -h` | Root filesystem 9.8 GiB at 81%; home filesystem 18 GiB at 67% |
| `python3 --version` | Python 3.13.5 |
| `git --version` | Git 2.47.3 |
| `codex --version` | Initially absent; after the user-local installation: Codex 0.147.0 |
| `codex features list` | After installation, Hooks and multi-agent support reported stable and enabled |
| `codex app-server --help` | Help available after installation; no observation client was attached |
| `arduino-cli version` | Arduino CLI 1.5.1 |
| `arduino-cli core list` | `arduino:zephyr` 0.90.0 installed |
| `systemctl status arduino-router --no-pager` | Active; Arduino Router 0.9.0 |
| `stat /var/run/arduino-router.sock` | Socket present with mode `0666` |
| `python3 -c 'import msgpack; print(msgpack.__version__)'` | Import initially failed because system Python had no `msgpack` package |
| `systemctl --failed` | Zero failed units |

The project uses a private virtual environment for MessagePack rather than
modifying system Python. A high-CPU pre-existing workload was present during
some Hook timing runs and was left untouched.

## Arduino core, matrix, and firmware

- The board target is `arduino:zephyr:unoq` from `arduino:zephyr` core 0.90.0.
- The onboard display is an 8 by 13 blue LED matrix controlled by the
  STM32U585. The current API used by the project is `matrix.begin()`,
  `matrix.setGrayscaleBits(3)`, and `matrix.draw(frame)`. The hardware exposes
  levels 0 through 7; the project caps normal configured brightness at 5.
- `Arduino_RouterBridge` 0.4.3 and its Arduino-library dependencies were
  installed in the user's Arduino library directory.
- The pre-existing STM32 application could not be identified or read back, so
  no automatic restoration claim is made.
- The firmware compiled successfully before upload with:

  ```bash
  arduino-cli compile --warnings all -b arduino:zephyr:unoq firmware/unoq_codex_matrix
  arduino-cli upload -b arduino:zephyr:unoq firmware/unoq_codex_matrix
  ```

  The build used 85,576 bytes of flash and 31,075 bytes of global RAM. Upload
  then succeeded through the board's official STM32U585/OpenOCD path.
- After upload, Router RPC returned protocol version 1 and firmware version
  0.1.0. The daemon republished IDLE after the MCU restart caused by flashing.

The official board workflow and layout are documented in the
[Arduino UNO Q user manual source](https://github.com/arduino/docs-content/blob/main/content/hardware/02.uno/boards/uno-q/tutorials/01.user-manual/content.md).

## Arduino Router and Bridge API

[Arduino Router](https://github.com/arduino/arduino-router) 0.9.0 remained
active throughout the live tests. Router uses MessagePack RPC in a star
topology. Its [published RPC framing](https://github.com/arduino/arduino-router/blob/main/msgpackrpc/README.md)
is:

```text
request  [0, message_id, method, params]
response [1, message_id, error, result]
```

No standalone official Python Bridge wrapper suitable for the daemon was found.
The project therefore implements the documented MessagePack RPC envelope over
the Router Unix socket. It never opens `/dev/ttyHS1` or uses STM32 `Serial1`.

The reviewed
[`Arduino_RouterBridge` 0.4.3 header](https://github.com/arduino-libraries/Arduino_RouterBridge/blob/0.4.3/src/bridge.h)
provides `Bridge.begin()` and `Bridge.provide_safe()`. Safe callbacks execute in
Arduino loop context. This firmware limits them to validation and bounded
pending-value updates; actual frame rendering remains in `loop()`.

The live RPC pass covered version, status, state, heartbeat, and brightness.
All state IDs were accepted, brightness 0 through 5 was returned exactly, and
inputs 6 and 7 were clamped to 5. A 1,000-update sequence completed without an
RPC failure. The Router service itself was not stopped because that could affect
other board workloads; transport-failure behavior is covered by isolated tests.

## Codex configuration, Hooks, and event schema

At the pre-change check, `~/.codex/hooks.json` and `~/.codex/config.toml` did not
exist. Later edits used timestamped backups whenever a target file existed, and
the project installers preserved unrelated Hook entries. The authentication
file's contents were never read. `codex login status` reported an existing
ChatGPT login without exposing credential material.

The Hook definitions were reviewed and trusted through `/hooks`; a persistent
`--dangerously-bypass-hook-trust` setting was not used. A temporary privacy-safe
probe recorded only the allowed metadata, then a production-path Hook was
tested. The following lifecycle events were observed on Codex 0.147.0:

| Event | Result |
|---|---|
| SessionStart | Observed |
| UserPromptSubmit | Observed |
| PreToolUse | Observed |
| PermissionRequest | Observed |
| PostToolUse | Observed |
| SubagentStart | Observed |
| SubagentStop | Observed |
| Stop | Observed |
| SessionEnd | Observed |
| PreCompact | Not forced or observed |
| PostCompact | Not forced or observed |

The reviewed representative records are in
`tests/fixtures/hooks/codex-0.147.0-uno-q.jsonl`. They contain event names,
bounded tool names, one-way hashes of identifier prefixes, event times, and
response value types only. The raw stdin, prompts, answers, commands, response
bodies, paths, environment, and unredacted identifiers were not retained.

For Bash and `apply_patch`, the observed `tool_response` type was a string. No
explicit structured exit code, `success: false`, failed status, or tool-error
field was available in those samples. In accordance with the privacy design,
the Hook did not inspect response prose, so an intentionally failing real test
did not emit ERROR. Synthetic structured failure fixtures do emit transient
ERROR and return to THINKING. This is a documented Codex 0.147.0 compatibility
limit, not a reason to parse response bodies.

A local non-interactive Codex smoke task ran in a dedicated disposable Git
repository. Without any LED instruction it exercised reading, file creation,
an intentional test failure, repair and passing test, a real permission request,
and a bounded subagent. The Hook/daemon pipeline observed the associated event
categories. In this Codex build, some file reads were implemented through Bash
and therefore classified as COMMAND rather than READING; a true Read-type event
was separately verified as READING.

After the final native executable replaced the portable probe command, all 11
definitions were reviewed again and shown active in `/hooks`. A fresh ordinary
`codex exec` read-only task (again with no LED instruction) observed
SessionStart, UserPromptSubmit, PreToolUse, PostToolUse, Stop, and the final
SessionEnd cleanup through the native Hook; daemon status returned to IDLE with
zero active sessions.

No second client was attached to App Server, so no `AppServerSource` behavior is
claimed. Phone-originated Remote execution still requires the user-device test
described in [remote-test.md](remote-test.md). A missing Remote Hook will not
automatically enable transcript or session-JSONL parsing.

## End-to-end hardware results

With the daemon manually running under the unprivileged board user, a normalized
event was propagated through the Unix datagram, session aggregator, MessagePack
RPC, Router, and flashed MCU. The observed logical state sequence covered IDLE,
THINKING, READING, WRITING, COMMAND, BUILDING, TESTING, FLASHING, WAITING,
SUBAGENT, transient ERROR, SUCCESS, and return to IDLE. OFF and OFFLINE were
also exercised through their control/heartbeat paths.

- SUCCESS remained visible after SessionEnd while the active-session count
  became zero, then expired to IDLE after the configured eight seconds.
- A structured transient ERROR returned to THINKING after approximately
  1.5 seconds.
- With daemon heartbeats absent for more than the configured timeout, firmware
  reported OFFLINE. Restarting the daemon restored IDLE automatically.
- The daemon restart, control path, status/doctor RPC, all-state demo, and rapid
  state updates completed successfully.

The continuous animation/RPC soak completed after 1,801.2 seconds without an RPC
failure. Sanitized progress samples recorded THINKING at minute 0, TESTING at
minute 5, OFFLINE at minute 10, WRITING at minute 15, WAITING at minute 20, and
IDLE at minute 25. The daemon returned the display to IDLE at completion.

These results establish logical transport and renderer state selection, not
human visual quality. No camera or local observer was available to confirm that
all animation patterns are physically distinct.

## Service-install constraint

The provided system-level unit and idempotent installer target
`User=arduino`, `RuntimeDirectory=unoq-codex-matrix`, and an ordering dependency
on `arduino-router.service`. The investigated account did not have
non-interactive administrative permission for the required `/opt`, `/etc`,
`/usr/local`, and systemd writes. No password was requested or handled, and
privilege settings were not changed. Consequently, the daemon was validated
manually with isolated runtime sockets, while privileged install/uninstall,
enable-at-boot, and a real system-service restart remain release checks.

## Hook latency qualification

The production primary is now a dependency-free native C Hook. On the real UNO
Q, with the unrelated high-CPU workload left running, each condition used 20
warmups followed by 200 measured processes:

| Condition | p95 | Maximum where recorded |
|---|---:|---:|
| daemon event socket absent | 3.513 ms | 3.707 ms |
| dummy Unix-datagram receiver | 3.543 ms | 3.724 ms |
| actual running daemon (final source; median 3.251 ms) | 3.615 ms | 5.540 ms |

The live-daemon path therefore met the required p95 below 50 ms with substantial
margin. The native suite also passed 47 of 47 integration cases under GCC 14
with `-Werror`, AddressSanitizer, and UndefinedBehaviorSanitizer. Additional
coverage sent 300 malformed random inputs and compared 500 valid cases with the
Python reference implementation.

At install time, `scripts/build-native-hook.sh` uses an existing `cc`, GCC, or
Clang when present. On the compiler-free investigated image it privately
downloaded and extracted Debian TCC and libc development files under a temporary
directory, built the Hook, and removed that directory. It did not install a
compiler package. The installer uses the `jq`/`socat` shell Hook only if this
native build fails.

The portable shell fallback passed functional fail-open tests but exceeded the
50 ms target in one daemon-running sample under the same high board load (p95
approximately 86.9 ms). It remains a compatibility fallback and does not inherit
the native Hook's latency qualification.

## GitHub tooling

GitHub CLI was not installed on the UNO Q. Publication uses the separately
authenticated development environment. At preflight, the requested repository
name did not already exist; creation, Actions, tag, and release are recorded by
the release itself rather than treated as device-investigation evidence.

## Changes and protected areas

Changes made for validation were limited to the new project directory,
user-local Codex and Arduino-library files, temporary isolated test files and
sockets, Codex Hook configuration with timestamped backups, and the intentional
STM32 firmware flash. No system package was removed and no OS-wide upgrade was
performed.

The work did not change xrdp, xorgxrdp, XFCE, X11, LightDM, network or SSH
configuration, Steam, FEX, Mesa/GPU/CEF settings, Cloudflare, kernel,
bootloader, existing Git repositories, or global Git configuration. The
pre-existing high-CPU workload was not stopped or reconfigured.

## Remaining device checks

The alpha intentionally keeps these items open:

- phone-originated Remote lifecycle-Hook behavior;
- privileged system install/uninstall and boot-time service operation; and
- human visual distinction of every physical animation and demo media.

The portable shell Hook's high-load latency is a documented fallback limitation,
not an uncompleted qualification of the native primary. App Server and a
deliberate live Router-service stop remain outside the v0.1 release checks.

Until these checks are complete, releases should retain an alpha version and
describe Remote as “awaiting device verification,” not “unsupported.”
