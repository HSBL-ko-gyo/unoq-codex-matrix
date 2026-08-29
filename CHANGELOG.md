# Changelog

All notable changes are documented here.

## [Unreleased]

- Added a bottom-row 13-segment Codex quota bar backed by the official
  app-server rate-limit method. It coexists with the upper-right active-session
  dots, hides when stale or unavailable, and remains backward compatible with
  pre-0.2.0 firmware.
- Smoothed THINKING with a 35 ms render cadence while preserving the 4.2 s
  logical lap, Q8 path and bubble coordinates, bilinear 3-bit intensity
  distribution, continuous trail decay, and temporally dithered residuals.
- Added a 420 ms whole-state fade-in, per-bubble 250-400 ms fade-in and
  200-350 ms fade-out, a bounded center envelope, and bounded transitions
  between IDLE, THINKING, SUCCESS, and IDLE while urgent states remain
  immediate.
- Added an MCU render-metrics RPC and measured the renderer plus matrix draw at
  82 us average and 158 us maximum during the 35 ms hardware animation.
- Expanded the sanitizer-backed renderer test to 200,000 frames with explicit
  subpixel energy conservation, fade lifecycle, transition interruption, and
  transition wraparound coverage.
- Expanded THINKING to a 52-point edge-to-edge infinity trajectory with a
  state-specific level-5 comet, four-sample tail, sparse moving residuals, and
  a clearer but restrained center-crossing response.
- Simplified the ambient bubble field to fixed integer x coordinates, smooth
  Q8 vertical rises, 480-720 ms spawning, constant level-one or level-two
  brightness, fade-only lifecycles, and saturating layer collisions.
- Replaced the two-pixel IDLE indicator with a four-pixel, 3.2-second breathing
  nucleus, a restrained 360 ms peak halo, and a 210 ms fade-in.
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
