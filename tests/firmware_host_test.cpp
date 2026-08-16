#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>

#include "animations.h"
#include "protocol.h"

using namespace unoq_codex_matrix;

namespace {

using GuardedFrame = std::array<uint8_t, kPixelCount + 2>;

uint8_t* pixels(GuardedFrame& frame) { return frame.data() + 1; }

GuardedFrame renderFrame(StateId state, uint32_t now_ms, uint32_t entered_ms,
                         uint8_t brightness, uint8_t active_count = 0,
                         bool show_active_count = false) {
  GuardedFrame frame;
  frame.fill(0xCC);
  frame.front() = 0xA5;
  frame.back() = 0x5A;
  renderAnimation(state, now_ms, entered_ms, brightness, active_count,
                  show_active_count, pixels(frame));
  return frame;
}

void assertGuardsAndLevels(const GuardedFrame& frame, StateId state,
                           uint8_t brightness) {
  assert(frame.front() == 0xA5);
  assert(frame.back() == 0x5A);
  const uint8_t maximum = state == THINKING
                              ? kMaxBrightness
                              : (brightness > kMaxBrightness ? kMaxBrightness
                                                              : brightness);
  for (uint16_t index = 1; index <= kPixelCount; ++index) {
    assert(frame[index] <= maximum);
  }
}

void renderChecked(StateId state, uint32_t now_ms, uint32_t entered_ms,
                   uint8_t brightness, uint8_t active_count,
                   bool show_active_count) {
  const GuardedFrame frame = renderFrame(state, now_ms, entered_ms, brightness,
                                         active_count, show_active_count);
  assertGuardsAndLevels(frame, state, brightness);
  if (brightness == 0 || state == OFF) {
    for (uint16_t index = 1; index <= kPixelCount; ++index) {
      assert(frame[index] == 0);
    }
  }
}

void assertThinkingRenderer() {
  bool reaches_left = false;
  bool reaches_right = false;
  bool reaches_top = false;
  bool reaches_bottom = false;
  bool center_reacts = false;
  bool saw_saturation = false;
  bool saw_spawn = false;
  bool saw_disappear = false;
  bool saw_pop = false;
  uint8_t minimum_bubbles = kThinkingBubbleCapacity;
  uint8_t maximum_bubbles = 0;
  std::array<uint8_t, 2> previous_head = {6, 4};

  // Exercise 100,000 frames (nearly two hours at 70 ms/frame), beginning
  // close to millis() rollover. Guard bytes make every out-of-bounds write a
  // deterministic test failure under both native and sanitizer builds.
  constexpr uint32_t kEntered =
      std::numeric_limits<uint32_t>::max() - 5000U;
  for (uint32_t sample = 0; sample < 100000U; ++sample) {
    const uint32_t elapsed = sample * kThinkingFrameIntervalMs;
    const uint32_t now = kEntered + elapsed;
    const GuardedFrame frame = renderFrame(THINKING, now, kEntered, 3);
    assertGuardsAndLevels(frame, THINKING, 3);
    const ThinkingDebugSnapshot debug = thinkingDebugSnapshot(elapsed);
    assert(debug.head_x < kMatrixWidth);
    assert(debug.head_y < kMatrixHeight);
    assert(debug.active_bubbles <= kThinkingBubbleCapacity);
    minimum_bubbles =
        debug.active_bubbles < minimum_bubbles ? debug.active_bubbles
                                                : minimum_bubbles;
    maximum_bubbles =
        debug.active_bubbles > maximum_bubbles ? debug.active_bubbles
                                                : maximum_bubbles;
    saw_spawn = saw_spawn || debug.spawn_mask != 0;
    saw_pop = saw_pop || debug.pop_mask != 0;

    uint8_t lit = 0;
    uint8_t comet_bright_pixels = 0;
    for (uint8_t y = 0; y < kMatrixHeight; ++y) {
      for (uint8_t x = 0; x < kMatrixWidth; ++x) {
        const uint8_t level =
            frame[1 + static_cast<uint16_t>(y) * kMatrixWidth + x];
        saw_saturation = saw_saturation || level == kMaxBrightness;
        if (level == 0) {
          continue;
        }
        ++lit;
        if (level >= 4) {
          ++comet_bright_pixels;
        }
      }
    }
    // The frame remains sparse and calm even with the independent ambience.
    assert(lit >= 7);
    assert(lit <= 28);
    assert(comet_bright_pixels >= 2);

    const uint16_t head_index =
        1 + static_cast<uint16_t>(debug.head_y) * kMatrixWidth + debug.head_x;
    assert(frame[head_index] >= 5);
    const std::array<uint8_t, 2> head = {debug.head_x, debug.head_y};
    reaches_left = reaches_left || debug.head_x == 0;
    reaches_right = reaches_right || debug.head_x == kMatrixWidth - 1;
    reaches_top = reaches_top || debug.head_y == 0;
    reaches_bottom = reaches_bottom || debug.head_y == kMatrixHeight - 1;
    const uint8_t dx = head[0] > previous_head[0]
                           ? static_cast<uint8_t>(head[0] - previous_head[0])
                           : static_cast<uint8_t>(previous_head[0] - head[0]);
    const uint8_t dy = head[1] > previous_head[1]
                           ? static_cast<uint8_t>(head[1] - previous_head[1])
                           : static_cast<uint8_t>(previous_head[1] - head[1]);
    assert(dx <= 1 && dy <= 1);
    previous_head = head;

    const uint16_t center = 1 + 4 * kMatrixWidth + 6;
    const bool weak_neighbours =
        frame[center - 1] != 0 || frame[center + 1] != 0 ||
        frame[center - kMatrixWidth] != 0;
    center_reacts = center_reacts || (frame[center] != 0 && weak_neighbours);
  }
  assert(reaches_left && reaches_right && reaches_top && reaches_bottom);
  assert(center_reacts);
  assert(saw_saturation);
  assert(saw_spawn && saw_pop);
  assert(minimum_bubbles <= 3);
  assert(maximum_bubbles >= 6);

  // Scan at 1 ms resolution to prove fixed-capacity lifecycle transitions,
  // 320-590 ms aggregate spawn jitter, and at least one surface pop.
  uint8_t previous_active_mask = thinkingDebugSnapshot(0).active_mask;
  uint8_t previous_spawn_mask = 0;
  uint32_t previous_spawn_ms = 0;
  bool have_previous_spawn = false;
  bool saw_birth = false;
  uint16_t minimum_spawn_gap = 1000;
  uint16_t maximum_spawn_gap = 0;
  for (uint32_t elapsed = 0; elapsed < 20000; ++elapsed) {
    const ThinkingDebugSnapshot debug = thinkingDebugSnapshot(elapsed);
    const uint8_t born = static_cast<uint8_t>(
        debug.active_mask & static_cast<uint8_t>(~previous_active_mask));
    const uint8_t gone = static_cast<uint8_t>(
        previous_active_mask & static_cast<uint8_t>(~debug.active_mask));
    saw_birth = saw_birth || born != 0;
    saw_disappear = saw_disappear || gone != 0;
    saw_pop = saw_pop || debug.pop_mask != 0;

    const uint8_t spawn_edge = static_cast<uint8_t>(
        debug.spawn_mask & static_cast<uint8_t>(~previous_spawn_mask));
    if (spawn_edge != 0) {
      if (have_previous_spawn) {
        const uint16_t gap =
            static_cast<uint16_t>(elapsed - previous_spawn_ms);
        minimum_spawn_gap = gap < minimum_spawn_gap ? gap : minimum_spawn_gap;
        maximum_spawn_gap = gap > maximum_spawn_gap ? gap : maximum_spawn_gap;
      }
      previous_spawn_ms = elapsed;
      have_previous_spawn = true;
    }
    previous_active_mask = debug.active_mask;
    previous_spawn_mask = debug.spawn_mask;
  }
  assert(saw_birth && saw_disappear && saw_pop);
  assert(minimum_spawn_gap >= 300);
  assert(maximum_spawn_gap <= 600);

  // Direction changes pause on the same endpoint for one bounded step, then
  // retrace smoothly. There is no discontinuity at either reversal boundary.
  const uint32_t reverse_start =
      kThinkingForwardLapsBeforeReverse * kThinkingLapDurationMs;
  const ThinkingDebugSnapshot before_reverse =
      thinkingDebugSnapshot(reverse_start - 1U);
  const ThinkingDebugSnapshot at_reverse = thinkingDebugSnapshot(reverse_start);
  assert(!before_reverse.reverse && at_reverse.reverse);
  assert(before_reverse.head_x == at_reverse.head_x);
  assert(before_reverse.head_y == at_reverse.head_y);
  const uint32_t forward_resume = reverse_start + kThinkingLapDurationMs;
  const ThinkingDebugSnapshot before_resume =
      thinkingDebugSnapshot(forward_resume - 1U);
  const ThinkingDebugSnapshot at_resume =
      thinkingDebugSnapshot(forward_resume);
  assert(before_resume.reverse && !at_resume.reverse);
  assert(before_resume.head_x == at_resume.head_x);
  assert(before_resume.head_y == at_resume.head_y);

  // Rendering and the bubble PRNG are deterministic.
  assert(renderFrame(THINKING, 1234567U, 321U, 5) ==
         renderFrame(THINKING, 1234567U, 321U, 5));
  const ThinkingDebugSnapshot deterministic_a = thinkingDebugSnapshot(1234246U);
  const ThinkingDebugSnapshot deterministic_b = thinkingDebugSnapshot(1234246U);
  assert(deterministic_a.bubble_signature == deterministic_b.bubble_signature);
  assert(deterministic_a.active_mask == deterministic_b.active_mask);
  assert(deterministic_a.pop_mask == deterministic_b.pop_mask);

  // THINKING alone receives the 70 ms refresh; every other state keeps the
  // daemon-configured cadence.
  assert(effectiveFrameIntervalMs(THINKING, 100) == 70);
  assert(effectiveFrameIntervalMs(THINKING, 60) == 60);
  assert(effectiveFrameIntervalMs(READING, 100) == 100);
}

}  // namespace

int main() {
  assertThinkingRenderer();

  // Exercise every state, every 3-bit input level, active-count edge values,
  // and a 30-minute-equivalent animation timeline without wall-clock waiting.
  for (uint8_t state = OFF; state <= SUBAGENT; ++state) {
    for (uint8_t brightness = 0; brightness <= 7; ++brightness) {
      for (uint32_t now_ms = 0; now_ms <= 1'800'000UL; now_ms += 997UL) {
        renderChecked(static_cast<StateId>(state), now_ms, 0, brightness,
                      static_cast<uint8_t>((now_ms / 997UL) % 5), true);
      }
    }
  }

  // Rapid switching should not retain pixels from a preceding state.
  for (uint32_t transition = 0; transition < 1000; ++transition) {
    renderChecked(static_cast<StateId>(transition % 14), transition,
                  transition, static_cast<uint8_t>(transition % 8),
                  static_cast<uint8_t>(transition % 4), true);
  }

  // Animation elapsed-time arithmetic must be identical across millis() wrap.
  for (uint8_t state = OFF; state <= SUBAGENT; ++state) {
    GuardedFrame wrapped;
    GuardedFrame ordinary;
    wrapped.fill(0);
    ordinary.fill(0);
    wrapped.front() = ordinary.front() = 0xA5;
    wrapped.back() = ordinary.back() = 0x5A;
    renderAnimation(static_cast<StateId>(state), 25U,
                    std::numeric_limits<uint32_t>::max() - 24U, 5, 3, true,
                    pixels(wrapped));
    renderAnimation(static_cast<StateId>(state), 50U, 0U, 5, 3, true,
                    pixels(ordinary));
    assert(wrapped == ordinary);
  }

  // Unknown state IDs take the bounded OFFLINE rendering path.
  GuardedFrame invalid;
  GuardedFrame offline;
  invalid.fill(0);
  offline.fill(0);
  invalid.front() = offline.front() = 0xA5;
  invalid.back() = offline.back() = 0x5A;
  renderAnimation(static_cast<StateId>(255), 1234, 0, 5, 0, false,
                  pixels(invalid));
  renderAnimation(OFFLINE, 1234, 0, 5, 0, false, pixels(offline));
  assert(invalid == offline);
  return 0;
}
