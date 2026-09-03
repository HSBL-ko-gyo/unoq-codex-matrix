#ifndef UNOQ_CODEX_MATRIX_ANIMATIONS_H
#define UNOQ_CODEX_MATRIX_ANIMATIONS_H

#include <stdint.h>

#include "protocol.h"

namespace unoq_codex_matrix {

// Public renderer timing constants used by the host-side continuity tests.
// The path itself remains an implementation detail: it is a moving particle
// trajectory, never a persistently drawn infinity glyph.
constexpr uint8_t kThinkingPathPointCount = 40;
constexpr uint16_t kThinkingLapDurationMs = 4200;
constexpr uint8_t kThinkingForwardLapsBeforeReverse = 9;
constexpr uint8_t kThinkingBubbleCapacity = 7;
// ArduinoCore-zephyr plays its native grayscale animation at 16 ms/frame.
constexpr uint16_t kThinkingFrameIntervalMs = 16;
constexpr uint16_t kThinkingFadeInMs = 420;
constexpr uint16_t kThinkingFadeOutMs = 210;
constexpr uint16_t kIdleFadeInMs = 210;
constexpr uint16_t kIdleFadeFrameIntervalMs = 32;
constexpr uint16_t kIdleStaticRefreshIntervalMs = 750;
constexpr uint8_t kQuotaBlinkThresholdPercent = 10;
constexpr uint16_t kQuotaBlinkHalfPeriodMs = 700;
constexpr uint16_t kQuotaBlinkFrameIntervalMs = 100;

constexpr bool shouldBlinkQuota(const bool show_quota_bar,
                                const uint8_t remaining_percent) {
  return show_quota_bar && remaining_percent > 0 &&
         remaining_percent <= kQuotaBlinkThresholdPercent;
}

// The production renderer uses the same 16 ms cadence as Arduino's native
// boot animation.  Other state timing remains daemon-configurable.
constexpr uint16_t effectiveFrameIntervalMs(const StateId state,
                                             const uint16_t configured_ms) {
  return state == THINKING && configured_ms > kThinkingFrameIntervalMs
             ? kThinkingFrameIntervalMs
             : configured_ms;
}

#if defined(UNOQ_CODEX_MATRIX_HOST_TEST)
struct ThinkingDebugSnapshot {
  uint16_t head_x_q8;
  uint16_t head_y_q8;
  uint8_t active_bubbles;
  uint8_t active_mask;
  uint8_t spawn_mask;
  uint8_t pop_mask;
  uint8_t thinking_opacity_q8;
  uint8_t center_opacity_q8;
  uint8_t bubble0_opacity_q8;
  uint8_t bubble0_brightness;
  uint16_t bubble0_x_q8;
  uint16_t bubble0_y_q8;
  uint16_t bubble0_lifetime_ms;
  uint16_t bubble0_fade_in_ms;
  uint16_t bubble0_fade_out_ms;
  uint32_t bubble_signature;
  bool reverse;
};

struct IdleDebugSnapshot {
  uint8_t fade_opacity_q8;
};

// Bounded renderer telemetry for host tests only. This is omitted from the
// STM32 firmware and does not alter the production protocol.
ThinkingDebugSnapshot thinkingDebugSnapshot(uint32_t elapsed_ms);
IdleDebugSnapshot idleDebugSnapshot(uint32_t elapsed_ms);

// Host-only primitive used to verify subpixel energy conservation and
// saturation independently of the complete animation layers.
void addThinkingTestParticle(uint16_t x_q8, uint16_t y_q8,
                             uint16_t intensity_q8, uint8_t temporal_phase,
                             uint8_t frame[kPixelCount]);
#endif

constexpr bool shouldCrossfadeThinkingTransition(const StateId from,
                                                  const StateId to) {
  return (from == IDLE && to == THINKING) ||
         (from == THINKING && (to == SUCCESS || to == IDLE));
}

constexpr uint16_t transitionDurationMs(const StateId from,
                                        const StateId to) {
  return from == IDLE && to == THINKING ? kThinkingFadeInMs
                                        : kThinkingFadeOutMs;
}

// Renders exactly one 8x13 grayscale frame. The caller owns the static frame
// buffer; this function performs no dynamic allocation.
void renderAnimation(StateId state, uint32_t now_ms, uint32_t state_entered_ms,
                     uint8_t brightness, uint8_t active_count,
                     bool show_active_count, uint8_t quota_remaining_percent,
                     bool show_quota_bar, uint8_t frame[kPixelCount]);

// Renders the bounded IDLE -> THINKING and THINKING -> SUCCESS/IDLE
// crossfades. Urgent states bypass this path in the firmware state machine.
void renderTransition(StateId from, StateId to, uint32_t now_ms,
                      uint32_t from_state_entered_ms,
                      uint32_t transition_started_ms, uint8_t brightness,
                      uint8_t active_count, bool show_active_count,
                      uint8_t quota_remaining_percent, bool show_quota_bar,
                      uint8_t frame[kPixelCount]);

}  // namespace unoq_codex_matrix

#endif  // UNOQ_CODEX_MATRIX_ANIMATIONS_H
