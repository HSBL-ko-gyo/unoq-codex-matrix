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
  GuardedFrame frame;
  frame.fill(0xCC);
  frame.front() = 0xA5;
  frame.back() = 0x5A;
  renderAnimation(state, now_ms, entered_ms, brightness, active_count,
                  show_active_count, pixels(frame));
  assertGuardsAndLevels(frame, brightness);
  if (brightness == 0 || state == OFF) {
    for (uint16_t index = 1; index <= kPixelCount; ++index) {
      assert(frame[index] == 0);
    }
  }
}

}  // namespace

int main() {
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
