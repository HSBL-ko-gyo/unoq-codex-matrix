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

uint16_t frameEnergy(const GuardedFrame& frame) {
  uint16_t energy = 0;
  for (uint16_t index = 1; index <= kPixelCount; ++index) {
    energy = static_cast<uint16_t>(energy + frame[index]);
  }
  return energy;
}

uint16_t absoluteDifference(const uint16_t first, const uint16_t second) {
  return first > second ? static_cast<uint16_t>(first - second)
                        : static_cast<uint16_t>(second - first);
}

GuardedFrame renderTransitionFrame(StateId from, StateId to, uint32_t now_ms,
                                   uint32_t from_entered_ms,
                                   uint32_t transition_started_ms,
                                   uint8_t brightness) {
  GuardedFrame frame;
  frame.fill(0xCC);
  frame.front() = 0xA5;
  frame.back() = 0x5A;
  renderTransition(from, to, now_ms, from_entered_ms, transition_started_ms,
                   brightness, 0, false, pixels(frame));
  return frame;
}

void assertThinkingRenderer() {
  bool reaches_left = false;
  bool reaches_right = false;
  bool reaches_top = false;
  bool reaches_bottom = false;
  bool center_reacts = false;
  bool saw_spawn = false;
  bool saw_disappear = false;
  bool saw_pop = false;
  uint8_t minimum_bubbles = kThinkingBubbleCapacity;
  uint8_t maximum_bubbles = 0;
  std::array<uint16_t, 2> previous_head = {
      thinkingDebugSnapshot(kThinkingFadeInMs).head_x_q8,
      thinkingDebugSnapshot(kThinkingFadeInMs).head_y_q8};

  // Global fade-in is monotonic in Q8 and starts completely dark.
  const ThinkingDebugSnapshot fade_0 = thinkingDebugSnapshot(0);
  const ThinkingDebugSnapshot fade_100 = thinkingDebugSnapshot(100);
  const ThinkingDebugSnapshot fade_200 = thinkingDebugSnapshot(200);
  const ThinkingDebugSnapshot fade_full =
      thinkingDebugSnapshot(kThinkingFadeInMs);
  assert(fade_0.thinking_opacity_q8 == 0);
  assert(fade_0.thinking_opacity_q8 < fade_100.thinking_opacity_q8);
  assert(fade_100.thinking_opacity_q8 < fade_200.thinking_opacity_q8);
  assert(fade_full.thinking_opacity_q8 == 255);
  assert(frameEnergy(renderFrame(THINKING, 0, 0, 3)) == 0);
  assert(frameEnergy(renderFrame(THINKING, kThinkingFadeInMs, 0, 3)) > 0);

  // Three 35 ms renders occur during the first 105 ms logical segment. The
  // head therefore occupies true intermediate Q8 positions, not only anchors.
  const ThinkingDebugSnapshot head_0 = thinkingDebugSnapshot(0);
  const ThinkingDebugSnapshot head_35 = thinkingDebugSnapshot(35);
  const ThinkingDebugSnapshot head_70 = thinkingDebugSnapshot(70);
  assert(head_0.head_y_q8 == 4 * 256u);
  assert(head_35.head_y_q8 < head_0.head_y_q8);
  assert(head_35.head_y_q8 > head_70.head_y_q8);
  assert((head_35.head_y_q8 & 0xFFu) != 0);

  // Bubble slot zero fades in, moves continuously upward, and fades out to
  // zero before its active bit disappears.
  assert(fade_0.bubble0_opacity_q8 == 0);
  const ThinkingDebugSnapshot bubble_mid_fade =
      thinkingDebugSnapshot(fade_0.bubble0_fade_in_ms / 2u);
  const ThinkingDebugSnapshot bubble_active =
      thinkingDebugSnapshot(fade_0.bubble0_fade_in_ms);
  const ThinkingDebugSnapshot bubble_fading_out = thinkingDebugSnapshot(
      fade_0.bubble0_lifetime_ms - fade_0.bubble0_fade_out_ms / 2u);
  const ThinkingDebugSnapshot bubble_gone =
      thinkingDebugSnapshot(fade_0.bubble0_lifetime_ms);
  assert(bubble_mid_fade.bubble0_opacity_q8 > 0);
  assert(bubble_mid_fade.bubble0_opacity_q8 < 255);
  assert(bubble_active.bubble0_opacity_q8 == 255);
  assert(bubble_active.bubble0_y_q8 < fade_0.bubble0_y_q8);
  assert(bubble_fading_out.bubble0_opacity_q8 > 0);
  assert(bubble_fading_out.bubble0_opacity_q8 < 255);
  assert(bubble_gone.bubble0_opacity_q8 == 0);
  assert((bubble_gone.active_mask & 1u) == 0);

  // Exercise 200,000 frames (almost two hours at 35 ms/frame), beginning
  // close to millis() rollover. Guard bytes make every out-of-bounds write a
  // deterministic test failure under both native and sanitizer builds.
  constexpr uint32_t kEntered =
      std::numeric_limits<uint32_t>::max() - 5000U;
  for (uint32_t sample = 0; sample < 200000U; ++sample) {
    const uint32_t elapsed =
        kThinkingFadeInMs + sample * kThinkingFrameIntervalMs;
    const uint32_t now = kEntered + elapsed;
    const GuardedFrame frame = renderFrame(THINKING, now, kEntered, 3);
    assertGuardsAndLevels(frame, THINKING, 3);
    const ThinkingDebugSnapshot debug = thinkingDebugSnapshot(elapsed);
    assert(debug.head_x_q8 >= 1u * 256u);
    assert(debug.head_x_q8 <= 11u * 256u);
    assert(debug.head_y_q8 >= 1u * 256u);
    assert(debug.head_y_q8 <= 6u * 256u);
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
    for (uint8_t y = 0; y < kMatrixHeight; ++y) {
      for (uint8_t x = 0; x < kMatrixWidth; ++x) {
        const uint8_t level =
            frame[1 + static_cast<uint16_t>(y) * kMatrixWidth + x];
        if (level == 0) {
          continue;
        }
        ++lit;
      }
    }
    // The frame remains sparse and calm even with the independent ambience.
    assert(lit >= 5);
    assert(lit <= 45);
    assert(frameEnergy(frame) >= 8);

    const std::array<uint16_t, 2> head = {debug.head_x_q8, debug.head_y_q8};
    reaches_left = reaches_left || debug.head_x_q8 == 1u * 256u;
    reaches_right = reaches_right || debug.head_x_q8 == 11u * 256u;
    reaches_top = reaches_top || debug.head_y_q8 == 1u * 256u;
    reaches_bottom = reaches_bottom || debug.head_y_q8 == 6u * 256u;
    const uint16_t dx = head[0] > previous_head[0]
                            ? static_cast<uint16_t>(head[0] - previous_head[0])
                            : static_cast<uint16_t>(previous_head[0] - head[0]);
    const uint16_t dy = head[1] > previous_head[1]
                            ? static_cast<uint16_t>(head[1] - previous_head[1])
                            : static_cast<uint16_t>(previous_head[1] - head[1]);
    assert(dx <= 128 && dy <= 128);
    previous_head = head;

    const uint16_t center = 1 + 4 * kMatrixWidth + 6;
    const bool weak_neighbours =
        frame[center - 1] != 0 || frame[center + 1] != 0 ||
        frame[center - kMatrixWidth] != 0;
    center_reacts = center_reacts || (frame[center] != 0 && weak_neighbours);
  }
  assert(reaches_left && reaches_right && reaches_top && reaches_bottom);
  assert(center_reacts);
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
  assert(absoluteDifference(before_reverse.head_x_q8, at_reverse.head_x_q8) <=
         3u);
  assert(absoluteDifference(before_reverse.head_y_q8, at_reverse.head_y_q8) <=
         3u);
  const uint32_t forward_resume = reverse_start + kThinkingLapDurationMs;
  const ThinkingDebugSnapshot before_resume =
      thinkingDebugSnapshot(forward_resume - 1U);
  const ThinkingDebugSnapshot at_resume =
      thinkingDebugSnapshot(forward_resume);
  assert(before_resume.reverse && !at_resume.reverse);
  assert(absoluteDifference(before_resume.head_x_q8, at_resume.head_x_q8) <=
         3u);
  assert(absoluteDifference(before_resume.head_y_q8, at_resume.head_y_q8) <=
         3u);

  // Rendering and the bubble PRNG are deterministic.
  assert(renderFrame(THINKING, 1234567U, 321U, 5) ==
         renderFrame(THINKING, 1234567U, 321U, 5));
  const ThinkingDebugSnapshot deterministic_a = thinkingDebugSnapshot(1234246U);
  const ThinkingDebugSnapshot deterministic_b = thinkingDebugSnapshot(1234246U);
  assert(deterministic_a.bubble_signature == deterministic_b.bubble_signature);
  assert(deterministic_a.active_mask == deterministic_b.active_mask);
  assert(deterministic_a.pop_mask == deterministic_b.pop_mask);

  // Bilinear subpixel distribution conserves average energy and saturates.
  uint32_t accumulated_energy = 0;
  for (uint16_t phase = 0; phase < 256; ++phase) {
    GuardedFrame particle;
    particle.fill(0);
    particle.front() = 0xA5;
    particle.back() = 0x5A;
    addThinkingTestParticle(4u * 256u + 128u, 3u * 256u + 128u,
                            5u * 256u, static_cast<uint8_t>(phase),
                            pixels(particle));
    assertGuardsAndLevels(particle, THINKING, 3);
    accumulated_energy += frameEnergy(particle);
  }
  assert(accumulated_energy == 5u * 256u);
  GuardedFrame saturated;
  saturated.fill(kMaxBrightness);
  saturated.front() = 0xA5;
  saturated.back() = 0x5A;
  addThinkingTestParticle(4u * 256u + 128u, 3u * 256u + 128u,
                          7u * 256u, 0, pixels(saturated));
  assertGuardsAndLevels(saturated, THINKING, 7);

  // Center excitation uses a bounded envelope rather than an on/off frame.
  assert(thinkingDebugSnapshot(0).center_opacity_q8 == 255);
  assert(thinkingDebugSnapshot(70).center_opacity_q8 >
         thinkingDebugSnapshot(140).center_opacity_q8);
  assert(thinkingDebugSnapshot(140).center_opacity_q8 == 0);

  // THINKING -> SUCCESS/IDLE crossfades for 210 ms. Urgent states interrupt.
  const GuardedFrame transition_start =
      renderTransitionFrame(THINKING, SUCCESS, 1000, 0, 1000, 3);
  const GuardedFrame thinking_at_start = renderFrame(THINKING, 1000, 0, 3);
  const GuardedFrame transition_middle =
      renderTransitionFrame(THINKING, SUCCESS, 1105, 0, 1000, 3);
  const GuardedFrame transition_end =
      renderTransitionFrame(THINKING, SUCCESS, 1210, 0, 1000, 3);
  const GuardedFrame success_first = renderFrame(SUCCESS, 1000, 1000, 3);
  assert(transition_start == thinking_at_start);
  assert(transition_middle != transition_start);
  assert(transition_middle != transition_end);
  assert(transition_end == success_first);
  assert(shouldCrossfadeThinkingTransition(THINKING, SUCCESS));
  assert(shouldCrossfadeThinkingTransition(THINKING, IDLE));
  assert(!shouldCrossfadeThinkingTransition(THINKING, WAITING));
  assert(!shouldCrossfadeThinkingTransition(THINKING, ERROR));
  assert(!shouldCrossfadeThinkingTransition(THINKING, OFFLINE));

  // THINKING alone receives the 35 ms refresh; every other state keeps the
  // daemon-configured cadence.
  assert(effectiveFrameIntervalMs(THINKING, 100) == 35);
  assert(effectiveFrameIntervalMs(THINKING, 30) == 30);
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

  // Crossfade timing uses the same unsigned arithmetic across millis wrap.
  const GuardedFrame wrapped_transition = renderTransitionFrame(
      THINKING, SUCCESS, 25U,
      std::numeric_limits<uint32_t>::max() - 974U,
      std::numeric_limits<uint32_t>::max() - 104U, 3);
  const GuardedFrame ordinary_transition =
      renderTransitionFrame(THINKING, SUCCESS, 1000U, 0U, 870U, 3);
  assert(wrapped_transition == ordinary_transition);

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
