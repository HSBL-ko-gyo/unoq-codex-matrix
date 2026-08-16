# Prior art and independent-design notes

## Scope and method

Six public projects were reviewed before implementation. The review focused on
process lifetime, local transport, multi-session behavior, permission and
completion states, failure isolation, and the information available through
Codex Micro-style HID transports. Links below are pinned to the revisions that
were examined so later upstream changes do not rewrite this record.

This is a behavioral and architectural study, not a code provenance chain. UNO
Q Codex Matrix was implemented independently against public Codex Hook and
Arduino Router interfaces. No prior-art source, binary, asset, VID/PID, or
protocol implementation was copied.

## Comparison

| Project | Hook lifetime and handoff | Session/state model | Permission/completion behavior | Failure isolation | License |
|---|---|---|---|---|---|
| [codex-kick75-status-lights](https://github.com/Pixelmoss/codex-kick75-status-lights/tree/e32648ee86a8a729734060ac09bd7f8a1213876f) | A small Hook opens a Unix stream connection with a bounded timeout and sends to a daemon. | Daemon keeps a session-indexed map and selects a state using priority and expiry. | Permission outranks running work; completion is held temporarily. | Connection errors are ignored and the Hook exits successfully; it emits `{}` where a blocking Hook response requires JSON. | [MIT](https://github.com/Pixelmoss/codex-kick75-status-lights/blob/e32648ee86a8a729734060ac09bd7f8a1213876f/LICENSE) |
| [codex-status-LED](https://github.com/GFlash6/codex-status-LED/tree/850c63531f5ad769b4ea658219d8e14b0f3f9acd) | The Hook forwards to a localhost HTTP hub and may start companion processes; its request timeout is much longer than this project's budget. | Uses a fixed client identity rather than aggregation of independent Codex sessions. | Has explicit visual completion steps; permission behavior is not a robust session-level wait model. | Local forwarding is separated from device output, but process startup and HTTP are unsuitable for a sub-50 ms Hook. | [MIT](https://github.com/GFlash6/codex-status-LED/blob/850c63531f5ad769b4ea658219d8e14b0f3f9acd/LICENSE) |
| [qmk-codex-status](https://github.com/k33bs/qmk-codex-status/tree/84bc3472c47ae29b36bf1163ef44b5012a0063c3) | No lifecycle Hook/Unix daemon pipeline; a desktop component sends Vendor HID reports to keyboard firmware. | A small set of colored/effect slots, not lifecycle sessions with turn/tool identity. | Host-selected slot appearance is transported; there is no Hook-native permission/completion contract. | Device transport depends on desktop and HID availability. | [GPL-2.0-or-later](https://github.com/k33bs/qmk-codex-status/blob/84bc3472c47ae29b36bf1163ef44b5012a0063c3/README.md#license) |
| [codex-micro-4-core2](https://github.com/imliubo/codex-micro-4-core2/tree/2ee23a4ab696f94bb78d250f28cc4a9b879ba079) | No Unix Hook daemon; BLE emulates a Vendor HID status device. | Six host-defined status slots carry appearance settings, not Codex lifecycle records. | The device renders the host's chosen slot/effect and cannot infer richer approval or tool state itself. | BLE disconnect behavior is device-specific; it does not make Hook execution fail-open. | [MIT](https://github.com/imliubo/codex-micro-4-core2/blob/2ee23a4ab696f94bb78d250f28cc4a9b879ba079/LICENSE) |
| [rp2040-zero-onboard-led-codex-light](https://github.com/hu619340515/rp2040-zero-onboard-led-codex-light/tree/a32ed283b8a64615e088a913835bbe4663973624) | A Hook sends local UDP and can use a file-backed queue for a native status process. | Uses a fixed/source-oriented model rather than true per-session aggregation. | Maps permission to a waiting state; completion includes a heuristic based on assistant-message text. | Local send failures are ignored and response-sensitive events emit `{}`. | No license file or grant detected |
| [codex_led_state](https://github.com/huxun1978/codex_led_state/tree/52b6242e41913efa055070afb96f2318ea6687a6) | No lifecycle Hook; browser/CDP-derived text length is converted into a status file and HTTP update. | Coarse THINK/DONE/IDLE state only; no multi-session model. | No structured permission or explicit tool-completion semantics. | Depends on browser page structure and an HTTP receiver. | No license file or grant detected |

Relevant implementation examples in the most directly comparable project are
the [small forwarding Hook](https://github.com/Pixelmoss/codex-kick75-status-lights/blob/e32648ee86a8a729734060ac09bd7f8a1213876f/src/codex_kick75_hook.py#L14-L37)
and its [session/state daemon](https://github.com/Pixelmoss/codex-kick75-status-lights/blob/e32648ee86a8a729734060ac09bd7f8a1213876f/src/codex_kick75_daemon.py#L42-L117).
Those links document the concepts reviewed; their source was not reused.

## Concepts adopted independently

- Keep synchronous Hook work small and hand state to a long-lived local daemon.
- Treat the observer as optional: a missing daemon must never block a Codex turn.
- Aggregate independent sessions with a deterministic priority and recency tie-break.
- Represent completion and transient errors with expiry, so stale terminal states do
  not obscure new work.
- Give permission waiting higher visual priority than ordinary activity.

The implementation here deliberately uses a non-blocking Unix datagram, a 4 KiB
privacy allow-listed event, no retry, exact duplicate keys, monotonic ordering,
and a stricter 50 ms Hook target. Those choices were derived from project
requirements and public platform interfaces rather than copied implementation.

## Concepts not adopted

- No localhost HTTP server, web dashboard, or MQTT endpoint.
- No Hook-time daemon launch, network request, retry, or five-second timeout.
- No default parsing of Codex session JSONL or browser/page scraping.
- No prompt, response, command, transcript, or tool-response body logging.
- No heuristic search of natural-language output for success or failure.
- No file queue for raw or normalized Hook payloads.
- No Vendor HID emulation, borrowed USB identifiers, or desktop compatibility shim.

## Codex Micro protocol limits

The reviewed Codex Micro-style implementations transport a small number of
host-defined status slots—commonly six—with properties such as slot ID, color,
brightness, effect, and speed. That is enough to reproduce a light appearance,
but it does not itself expose:

- lifecycle event names;
- session, turn, or tool-use identifiers;
- detailed tool categories;
- structured permission requests;
- explicit tool exit status or failure metadata; or
- a multi-session aggregation model.

The reviewed implementations also differ in details such as how an effect is
encoded, and depend on desktop/Vendor HID behavior that is not a stable UNO Q
Hook interface. Consequently, Codex Micro HID is not an input source in v0.1.
If explored later, it will remain an experimental event-source role (conceptually
`CodexMicroHidSource`) rather than replace the v0.1 lifecycle-Hook path.

## License conclusion

MIT-licensed projects were used only as conceptual references. The
GPL-2.0-or-later implementation was not incorporated. Repositories with no
detected license were treated as all-rights-reserved for reuse purposes, so no
code or assets were taken from them. See [THIRD_PARTY.md](../THIRD_PARTY.md) for
the concise notice included with the distribution.
