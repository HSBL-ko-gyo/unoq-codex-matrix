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

void assertGuardsAndLevels(const GuardedFrame& frame, uint8_t brightness) {
  assert(frame.front() == 0xA5);
  assert(frame.back() == 0x5A);
  const uint8_t maximum = brightness > kMaxBrightness ? kMaxBrightness : brightness;
  for (uint16_t index = 1; index <= kPixelCount; ++index) {
    assert(frame[index] <= maximum);
  }
}

void renderChecked(StateId state, uint32_t now_ms, uint32_t entered_ms,
                   uint8_t brightness, uint8_t active_count,
                   bool show_active_count) {
  const GuardedFrame frame = renderFrame(state, now_ms, entered_ms, brightness,
                                         active_count, show_active_count);
  assertGuardsAndLevels(frame, brightness);
  if (brightness == 0 || state == OFF) {
    for (uint16_t index = 1; index <= kPixelCount; ++index) {
      assert(frame[index] == 0);
    }
  }
}

std::array<uint8_t, 2> thinkingHead(const GuardedFrame& frame) {
  uint8_t count = 0;
  std::array<uint8_t, 2> coordinate = {0, 0};
  for (uint8_t y = 0; y < kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < kMatrixWidth; ++x) {
      if (frame[1 + static_cast<uint16_t>(y) * kMatrixWidth + x] ==
          kMaxBrightness) {
        coordinate = {x, y};
        ++count;
      }
    }
  }
  // At normal maximum brightness only the travelling head reaches level 5.
  assert(count == 1);
  return coordinate;
}

void assertThinkingRenderer() {
  bool reaches_left = false;
  bool reaches_right = false;
  bool reaches_top = false;
  bool reaches_bottom = false;
  bool center_reacts = false;
  std::array<uint8_t, 2> previous_head = {6, 4};

  // Exercise 100,000 frames (nearly three hours at 100 ms/frame), beginning
  // close to millis() rollover. Guard bytes make every out-of-bounds write a
  // deterministic test failure under both native and sanitizer builds.
  constexpr uint32_t kEntered =
      std::numeric_limits<uint32_t>::max() - 5000U;
  for (uint32_t sample = 0; sample < 100000U; ++sample) {
    const uint32_t now = kEntered + sample * 100U;
    const GuardedFrame frame = renderFrame(THINKING, now, kEntered, 5);
    assertGuardsAndLevels(frame, 5);

    uint8_t lit = 0;
    for (uint8_t y = 0; y < kMatrixHeight; ++y) {
      for (uint8_t x = 0; x < kMatrixWidth; ++x) {
        const uint8_t level =
            frame[1 + static_cast<uint16_t>(y) * kMatrixWidth + x];
        if (level == 0) {
          continue;
        }
        ++lit;
        reaches_left = reaches_left || x <= 1;
        reaches_right = reaches_right || x >= 11;
        reaches_top = reaches_top || y <= 1;
        reaches_bottom = reaches_bottom || y >= 6;
      }
    }
    // The trajectory must remain sparse: particles move on an infinity path,
    // but the infinity outline itself is never rendered.
    assert(lit >= 4);
    assert(lit <= 12);

    const std::array<uint8_t, 2> head = thinkingHead(frame);
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

  // Direction changes pause on the same endpoint for one bounded step, then
  // retrace smoothly. There is no discontinuity at either reversal boundary.
  const uint32_t reverse_start =
      kThinkingForwardLapsBeforeReverse * kThinkingLapDurationMs;
  const GuardedFrame before_reverse =
      renderFrame(THINKING, reverse_start - 1U, 0, 5);
  const GuardedFrame at_reverse = renderFrame(THINKING, reverse_start, 0, 5);
  assert(thinkingHead(before_reverse) == thinkingHead(at_reverse));
  const uint32_t forward_resume = reverse_start + kThinkingLapDurationMs;
  const GuardedFrame before_resume =
      renderFrame(THINKING, forward_resume - 1U, 0, 5);
  const GuardedFrame at_resume = renderFrame(THINKING, forward_resume, 0, 5);
  assert(thinkingHead(before_resume) == thinkingHead(at_resume));

  // Rendering is deterministic, including the slow phase drift.
  assert(renderFrame(THINKING, 1234567U, 321U, 5) ==
         renderFrame(THINKING, 1234567U, 321U, 5));
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
