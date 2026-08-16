# Hardware validation history

This document contains point-in-time validation records. A snapshot describes what was exercised on the stated date and revision; it is not a claim about the latest `main` branch or a substitute for current release notes.

## 2026-08-16 validation snapshot

- **Date:** 2026-08-16
- **Validated against:** tag `v0.1.0-alpha.1`, commit `c197557`
- **Hardware:** Arduino UNO Q with the onboard STM32U585-controlled 8x13 LED matrix
- **Host:** Debian 13 on AArch64, Linux kernel 7.0.0

### Hardware and software versions

| Component | Version observed in this snapshot |
|---|---|
| Codex | 0.147.0, official Linux ARM64 build |
| Python | 3.13.5 |
| Arduino CLI | 1.5.1 |
| Arduino Router | 0.9.0 |
| Arduino UNO Q Zephyr core | `arduino:zephyr` 0.90.0 |
| Arduino_RouterBridge | 0.4.3 |
| Firmware | 0.1.0 |
| Wire protocol | 1 |

### Observed Hooks

`SessionStart`, `UserPromptSubmit`, `PreToolUse`, `PermissionRequest`, `PostToolUse`, `SubagentStart`, `SubagentStop`, `Stop`, and `SessionEnd` were observed. `PreCompact` and `PostCompact` were not forced during this snapshot.

A dedicated local Codex smoke task contained no LED instruction and exercised Hook-driven state changes, including a real permission request and subagent lifecycle.

### End-to-end and RPC results

- Compiled and uploaded the STM32U585 firmware through `arduino-cli`; Router RPC returned protocol version 1 and firmware version 0.1.0.
- Exercised every state ID, brightness clamping, SUCCESS and transient-ERROR expiry, daemon restart, heartbeat timeout to OFFLINE, and recovery.
- Completed 1,000 rapid state updates without an RPC failure.
- Completed a 1,801.2-second continuous hardware animation/RPC soak without an RPC failure. Samples covered THINKING, TESTING, OFFLINE, WRITING, WAITING, and IDLE.

These results establish transport and renderer state selection. They do not establish the visual quality or physical distinctness of every animation.

### Hook latency and robustness

The board's unrelated high-CPU workload remained active. Each timing condition used 20 warmups followed by 200 measured native-Hook processes.

| Condition | p95 | Maximum where recorded |
|---|---:|---:|
| Daemon event socket absent | 3.513 ms | 3.707 ms |
| Dummy Unix-datagram receiver | 3.543 ms | 3.724 ms |
| Running daemon (median 3.285 ms) | 3.573 ms | 3.746 ms |

The native suite passed 47 integration cases under GCC 14 with `-Werror`, AddressSanitizer, and UndefinedBehaviorSanitizer. It also passed 300 malformed random inputs and 500 valid differential cases against the Python reference. The portable shell fallback passed functional fail-open tests but recorded approximately 86.9 ms p95 in one daemon-running sample under the same load.

### Version-specific observations

In the observed Codex build, Bash and `apply_patch` supplied string `tool_response` values without a structured exit code or failed status. Because the Hook does not inspect response prose, an intentionally failing real command did not emit ERROR; synthetic inputs with explicit structured failure fields did. Some file reads were performed through Bash and were therefore classified as COMMAND rather than READING.

These observations describe compatibility at this snapshot only. They do not change the privacy rule against parsing response or command contents for failure text.

### Known limitations at this snapshot

- Phone-originated Remote lifecycle-Hook behavior was not exercised.
- The system service was not installed because non-interactive administrative access was unavailable; the daemon and control sockets were exercised manually.
- No local observer or camera was available to confirm that every physical animation was visually distinct.
- App Server input and a deliberate live Arduino Router service stop were outside the validation scope.

For the fuller sanitized device record, including baseline commands and investigation boundaries, see [UNO Q investigation](investigation.md).
