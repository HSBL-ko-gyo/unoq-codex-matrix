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
  uint8_t minimum_bubbles = kThinkingBubbleCapacity;
  uint8_t maximum_bubbles = 0;
  uint32_t bubble_count_total = 0;
  uint32_t bubble_count_samples = 0;
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
  assert(fade_0.bubble0_x_q8 == bubble_mid_fade.bubble0_x_q8);
  assert(fade_0.bubble0_x_q8 == bubble_active.bubble0_x_q8);
  assert((fade_0.bubble0_x_q8 & 0xFFu) == 0);
  assert(fade_0.bubble0_brightness >= 1);
  assert(fade_0.bubble0_brightness <= 2);
  assert(bubble_active.bubble0_y_q8 < fade_0.bubble0_y_q8);
  assert(bubble_fading_out.bubble0_opacity_q8 > 0);
  assert(bubble_fading_out.bubble0_opacity_q8 < 255);
  assert(bubble_gone.bubble0_opacity_q8 == 0);
  assert((bubble_gone.active_mask & 1u) == 0);

  // During a complete slot-zero lifetime, x never changes and y rises
  // monotonically (decreasing matrix row) with Q8 intermediate positions.
  uint16_t previous_bubble_y = fade_0.bubble0_y_q8;
  bool saw_fractional_bubble_y = false;
  for (uint32_t elapsed = 1; elapsed < fade_0.bubble0_lifetime_ms;
       elapsed += 17) {
    const ThinkingDebugSnapshot debug = thinkingDebugSnapshot(elapsed);
    assert(debug.bubble0_x_q8 == fade_0.bubble0_x_q8);
    assert(debug.bubble0_y_q8 <= previous_bubble_y);
    assert(debug.bubble0_brightness == fade_0.bubble0_brightness);
    assert(debug.bubble0_brightness <= 2);
    saw_fractional_bubble_y =
        saw_fractional_bubble_y || (debug.bubble0_y_q8 & 0xFFu) != 0;
    previous_bubble_y = debug.bubble0_y_q8;
  }
  assert(saw_fractional_bubble_y);

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
    bubble_count_total += debug.active_bubbles;
    ++bubble_count_samples;
    saw_spawn = saw_spawn || debug.spawn_mask != 0;
    assert(debug.pop_mask == 0);

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
    assert(lit >= 3);
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
  assert(saw_spawn);
  assert(minimum_bubbles >= 2);
  assert(maximum_bubbles <= 5);
  const uint32_t average_bubbles_x100 =
      (bubble_count_total * 100u) / bubble_count_samples;
  assert(average_bubbles_x100 >= 300u);
  assert(average_bubbles_x100 <= 400u);

  // Scan at 1 ms resolution to prove fixed-capacity lifecycle transitions,
  // 400-800 ms aggregate spawn jitter, and the absence of any surface pop.
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
    assert(debug.pop_mask == 0);

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
  assert(saw_birth && saw_disappear);
  assert(minimum_spawn_gap >= 400);
  assert(maximum_spawn_gap <= 800);

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

void assertIdleRenderer() {
  const IdleDebugSnapshot start = idleDebugSnapshot(0);
  const IdleDebugSnapshot quarter = idleDebugSnapshot(800);
  const IdleDebugSnapshot peak = idleDebugSnapshot(1600);
  const IdleDebugSnapshot three_quarters = idleDebugSnapshot(2400);
  const IdleDebugSnapshot wrapped = idleDebugSnapshot(kIdleBreathPeriodMs);
  assert(start.breath_q8 == 0);
  assert(start.halo_q8 == 0);
  assert(start.fade_opacity_q8 == 0);
  assert(quarter.breath_q8 > start.breath_q8);
  assert(peak.breath_q8 == 255);
  assert(peak.halo_q8 == 255);
  assert(three_quarters.breath_q8 == quarter.breath_q8);
  assert(wrapped.breath_q8 == start.breath_q8);
  assert(idleDebugSnapshot(kIdleFadeInMs).fade_opacity_q8 == 255);

  bool saw_quiet_phase = false;
  bool saw_peak_halo = false;
  for (uint32_t elapsed = 0; elapsed < 10u * kIdleBreathPeriodMs;
       elapsed += kThinkingFrameIntervalMs) {
    const GuardedFrame frame = renderFrame(IDLE, elapsed, 0, 7);
    assertGuardsAndLevels(frame, IDLE, 2);
    uint8_t lit = 0;
    for (uint16_t index = 1; index <= kPixelCount; ++index) {
      assert(frame[index] <= 2);
      lit = static_cast<uint8_t>(lit + (frame[index] != 0 ? 1 : 0));
    }
    if (elapsed >= kIdleFadeInMs &&
        idleDebugSnapshot(elapsed).halo_q8 == 0) {
      saw_quiet_phase = saw_quiet_phase || (lit >= 3 && lit <= 4);
    }
    if (idleDebugSnapshot(elapsed).halo_q8 > 220) {
      saw_peak_halo = saw_peak_halo || lit >= 5;
    }
  }
  assert(saw_quiet_phase);
  assert(saw_peak_halo);

  // The small nucleus recedes while THINKING completes its existing 420 ms
  // fade. The endpoint is a fully advanced THINKING frame, not a restart.
  const GuardedFrame idle_to_thinking_start =
      renderTransitionFrame(IDLE, THINKING, 5000, 0, 5000, 3);
  const GuardedFrame idle_before_transition = renderFrame(IDLE, 5000, 0, 3);
  const GuardedFrame idle_to_thinking_middle =
      renderTransitionFrame(IDLE, THINKING, 5210, 0, 5000, 3);
  const GuardedFrame idle_to_thinking_end =
      renderTransitionFrame(IDLE, THINKING, 5420, 0, 5000, 3);
  const GuardedFrame thinking_faded_in = renderFrame(THINKING, 5420, 5000, 3);
  assert(idle_to_thinking_start == idle_before_transition);
  assert(idle_to_thinking_middle != idle_to_thinking_start);
  assert(idle_to_thinking_middle != idle_to_thinking_end);
  assert(idle_to_thinking_end == thinking_faded_in);
  assert(shouldCrossfadeThinkingTransition(IDLE, THINKING));
  assert(transitionDurationMs(IDLE, THINKING) == kThinkingFadeInMs);

  // THINKING still reaches SUCCESS through the 210 ms crossfade; a later
  // IDLE state then emerges over its own bounded 210 ms fade-in.
  const GuardedFrame success =
      renderTransitionFrame(THINKING, SUCCESS, 8210, 0, 8000, 3);
  assert(success == renderFrame(SUCCESS, 8000, 8000, 3));
  assert(frameEnergy(renderFrame(IDLE, 9000, 9000, 3)) == 0);
  assert(frameEnergy(renderFrame(IDLE, 9210, 9000, 3)) > 0);
  assert(transitionDurationMs(THINKING, SUCCESS) == kThinkingFadeOutMs);
}

}  // namespace

int main() {
  assertThinkingRenderer();
  assertIdleRenderer();

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
