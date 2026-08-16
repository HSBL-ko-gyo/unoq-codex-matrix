#ifndef UNOQ_CODEX_MATRIX_ANIMATIONS_H
#define UNOQ_CODEX_MATRIX_ANIMATIONS_H

#include <stdint.h>

#include "protocol.h"

namespace unoq_codex_matrix {

// Public renderer timing constants used by the host-side continuity tests.
// The path itself remains an implementation detail: it is a moving particle
// trajectory, never a persistently drawn infinity glyph.
constexpr uint8_t kThinkingPathPointCount = 52;
constexpr uint16_t kThinkingLapDurationMs = 4200;
constexpr uint8_t kThinkingForwardLapsBeforeReverse = 9;
constexpr uint8_t kThinkingBubbleCapacity = 7;
constexpr uint16_t kThinkingFrameIntervalMs = 70;

// THINKING can refresh more smoothly than the configured cadence without
// changing any other state's timing or the Router RPC contract.
constexpr uint16_t effectiveFrameIntervalMs(const StateId state,
                                             const uint16_t configured_ms) {
  return state == THINKING && configured_ms > kThinkingFrameIntervalMs
             ? kThinkingFrameIntervalMs
             : configured_ms;
}

#if defined(UNOQ_CODEX_MATRIX_HOST_TEST)
struct ThinkingDebugSnapshot {
  uint8_t head_x;
  uint8_t head_y;
  uint8_t active_bubbles;
  uint8_t active_mask;
  uint8_t spawn_mask;
  uint8_t pop_mask;
  uint32_t bubble_signature;
  bool reverse;
};

// Bounded renderer telemetry for host tests only. This is omitted from the
// STM32 firmware and does not alter the production protocol.
ThinkingDebugSnapshot thinkingDebugSnapshot(uint32_t elapsed_ms);
#endif

// Renders exactly one 8x13 grayscale frame. The caller owns the static frame
// buffer; this function performs no dynamic allocation.
void renderAnimation(StateId state, uint32_t now_ms, uint32_t state_entered_ms,
                     uint8_t brightness, uint8_t active_count,
                     bool show_active_count, uint8_t frame[kPixelCount]);

}  // namespace unoq_codex_matrix

#endif  // UNOQ_CODEX_MATRIX_ANIMATIONS_H
