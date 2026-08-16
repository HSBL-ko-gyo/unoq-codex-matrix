# Changelog

All notable changes are documented here.

## [Unreleased]

- Expanded THINKING to a 52-point edge-to-edge infinity trajectory with a
  state-specific level-5 comet, four-sample tail, sparse moving residuals, and
  a clearer but restrained center-crossing response.
- Added an independent fixed-capacity ambient bubble field with deterministic
  320-590 ms spawning, slow rise and lateral wobble, breathing grayscale,
  one-frame surface pops, and saturating layer collisions.
- Increased only THINKING to a 70 ms refresh, reduced reversal frequency to
  one lap per ten-lap cycle, and retained deterministic integer-only timing.
- Extended the GCC 14 `-Werror` sanitizer host test past 100,000 THINKING
  frames to cover bubble capacity and lifecycle, spawn cadence, surface pops,
  saturation, bounds, zero brightness, millis rollover, and reversal.

## [0.1.0-alpha.1] - 2026-08-16

- Initial independent implementation of the Codex lifecycle hook pipeline.
- Multi-session state aggregation with completion and error TTLs.
- Arduino Router MessagePack-RPC backend and UNO Q STM32U585 firmware.
- systemd integration, installer, CLI, tests, and bilingual documentation.
- Added an explicit `EventSource` boundary with the v0.1 `CodexHooksSource`.
- Verified local Codex 0.147.0 lifecycle events with privacy-reviewed fixtures.
- Added the dependency-free native C Hook as the low-latency primary, with an
  install-time private compiler fallback and the portable shell Hook retained as
  a compatibility fallback.
- Qualified the final native Hook at p95 3.573 ms against the live daemon and passed
  sanitizer, malformed-input, and Python differential coverage.
- Compiled and flashed the UNO Q firmware; verified RPC, all state IDs, TTLs,
  rapid updates, heartbeat OFFLINE behavior, and daemon recovery on hardware.
- Completed a 1,801.2-second hardware animation/RPC soak without a failure.
- Documented alpha limitations: phone-originated Remote is unverified,
  string-only tool responses do not support explicit failure classification,
  and privileged service installation plus physical visual checks remain open.
