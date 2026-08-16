#include "animations.h"

#include <string.h>

#include "frames.h"

namespace unoq_codex_matrix {
namespace {

inline uint8_t highLevel(const uint8_t brightness) {
  return brightness > kMaxBrightness ? kMaxBrightness : brightness;
}

inline uint8_t mediumLevel(const uint8_t brightness) {
  const uint8_t high = highLevel(brightness);
  return high == 0 ? 0 : static_cast<uint8_t>((high + 1) / 2);
}

inline uint8_t lowLevel(const uint8_t brightness) {
  return brightness == 0 ? 0 : 1;
}

inline uint8_t scaledLevel(const uint8_t brightness,
                           const uint8_t nominal_level) {
  const uint8_t high = highLevel(brightness);
  if (high == 0 || nominal_level == 0) {
    return 0;
  }
  const uint8_t scaled = static_cast<uint8_t>(
      (static_cast<uint16_t>(high) * nominal_level) / 7u);
  return scaled == 0 ? 1 : scaled;
}

inline void setPixel(uint8_t frame[kPixelCount], const int16_t x,
                     const int16_t y, const uint8_t level) {
  if (x < 0 || x >= kMatrixWidth || y < 0 || y >= kMatrixHeight) {
    return;
  }
  const uint16_t index = static_cast<uint16_t>(y) * kMatrixWidth +
                         static_cast<uint16_t>(x);
  const uint8_t safe_level = level > kMaxBrightness ? kMaxBrightness : level;
  if (frame[index] < safe_level) {
    frame[index] = safe_level;
  }
}

inline void addPixelSaturating(uint8_t frame[kPixelCount], const int16_t x,
                               const int16_t y, const uint8_t level) {
  if (x < 0 || x >= kMatrixWidth || y < 0 || y >= kMatrixHeight) {
    return;
  }
  const uint16_t index = static_cast<uint16_t>(y) * kMatrixWidth +
                         static_cast<uint16_t>(x);
  const uint16_t combined =
      static_cast<uint16_t>(frame[index]) + highLevel(level);
  frame[index] = combined > kMaxBrightness
                     ? kMaxBrightness
                     : static_cast<uint8_t>(combined);
}

void drawGlyph(uint8_t frame[kPixelCount], const uint16_t rows[kMatrixHeight],
               const uint8_t level) {
  for (uint8_t y = 0; y < kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < kMatrixWidth; ++x) {
      if ((rows[y] & (1u << x)) != 0) {
        setPixel(frame, x, y, level);
      }
    }
  }
}

uint8_t pingPongPosition(const uint32_t elapsed_ms, const uint16_t step_ms,
                         const uint8_t extent) {
  if (extent < 2 || step_ms == 0) {
    return 0;
  }
  const uint16_t leg = static_cast<uint16_t>(extent - 1);
  const uint16_t cycle = static_cast<uint16_t>(leg * 2);
  const uint16_t phase = static_cast<uint16_t>((elapsed_ms / step_ms) % cycle);
  return phase <= leg ? static_cast<uint8_t>(phase)
                      : static_cast<uint8_t>(cycle - phase);
}

void renderIdle(uint8_t frame[kPixelCount], const uint32_t elapsed_ms,
                const uint8_t brightness) {
  const uint8_t high = highLevel(brightness);
  if (high == 0) {
    return;
  }
  constexpr uint16_t kHalfBreathMs = 1200;
  const uint16_t phase = static_cast<uint16_t>(elapsed_ms % (2 * kHalfBreathMs));
  const uint16_t ramp = phase <= kHalfBreathMs ? phase : 2 * kHalfBreathMs - phase;
  const uint8_t level = static_cast<uint8_t>(
      1 + (static_cast<uint32_t>(high - 1) * ramp) / kHalfBreathMs);
  setPixel(frame, 6, 3, level);
  setPixel(frame, 6, 4, mediumLevel(level));
}

struct ThinkingPoint {
  int8_t x;
  int8_t y;
};

// A full-canvas discretized lemniscate. Consecutive entries are 8-neighbours,
// with no consecutive duplicates, so the comet never jumps or stalls. The two
// centre crossings intentionally share physical pixels half a lap apart.
constexpr ThinkingPoint kThinkingPath[kThinkingPathPointCount] = {
    {6, 4},  {6, 3},  {5, 3},  {5, 2},  {4, 2},  {4, 1},
    {3, 1},  {3, 0},  {2, 0},  {1, 0},  {1, 1},  {0, 1},
    {0, 2},  {0, 3},  {0, 4},  {0, 5},  {0, 6},  {1, 6},
    {1, 7},  {2, 7},  {3, 7},  {3, 6},  {4, 6},  {4, 5},
    {5, 5},  {5, 4},  {6, 4},  {6, 3},  {7, 3},  {7, 2},
    {8, 2},  {8, 1},  {9, 1},  {9, 0},  {10, 0}, {11, 0},
    {11, 1}, {12, 1}, {12, 2}, {12, 3}, {12, 4}, {12, 5},
    {12, 6}, {11, 6}, {11, 7}, {10, 7}, {9, 7},  {9, 6},
    {8, 6},  {8, 5},  {7, 5},  {7, 4},
};

// An integer-only velocity ripple keeps motion calm but avoids a metronome.
constexpr uint8_t kThinkingStepDurationMs[kThinkingPathPointCount] = {
    81, 79, 77, 79, 81, 83, 85, 83, 81, 79, 81, 79, 77,
    79, 81, 83, 85, 83, 81, 79, 81, 79, 77, 79, 81, 83,
    85, 83, 81, 79, 81, 79, 77, 79, 81, 83, 85, 83, 81,
    79, 81, 79, 77, 79, 81, 83, 85, 83, 81, 79, 81, 79,
};

constexpr uint16_t thinkingStepDurationTotal() {
  uint16_t total = 0;
  for (uint8_t i = 0; i < kThinkingPathPointCount; ++i) {
    total = static_cast<uint16_t>(total + kThinkingStepDurationMs[i]);
  }
  return total;
}

static_assert(thinkingStepDurationTotal() == kThinkingLapDurationMs,
              "THINKING lap duration must match its step table");

struct ThinkingPhase {
  uint8_t travel_step;
  uint8_t path_index;
  uint8_t lap_modulo;
  uint8_t step_fraction;
  bool reverse;
};

uint8_t wrapThinkingStep(int16_t step) {
  while (step < 0) {
    step = static_cast<int16_t>(step + kThinkingPathPointCount);
  }
  while (step >= kThinkingPathPointCount) {
    step = static_cast<int16_t>(step - kThinkingPathPointCount);
  }
  return static_cast<uint8_t>(step);
}

uint8_t thinkingPathIndex(const int16_t travel_step, const bool reverse) {
  const uint8_t wrapped = wrapThinkingStep(travel_step);
  return reverse ? static_cast<uint8_t>(kThinkingPathPointCount - 1 - wrapped)
                 : wrapped;
}

ThinkingPhase thinkingPhase(const uint32_t elapsed_ms) {
  const uint32_t lap = elapsed_ms / kThinkingLapDurationMs;
  uint16_t within_lap =
      static_cast<uint16_t>(elapsed_ms % kThinkingLapDurationMs);
  uint8_t travel_step = 0;
  while (travel_step + 1 < kThinkingPathPointCount &&
         within_lap >= kThinkingStepDurationMs[travel_step]) {
    within_lap = static_cast<uint16_t>(
        within_lap - kThinkingStepDurationMs[travel_step]);
    ++travel_step;
  }

  constexpr uint8_t kDirectionCycleLaps =
      kThinkingForwardLapsBeforeReverse + 1;
  const uint8_t lap_modulo =
      static_cast<uint8_t>(lap % kDirectionCycleLaps);
  const bool reverse = lap_modulo == kThinkingForwardLapsBeforeReverse;
  const uint8_t duration = kThinkingStepDurationMs[travel_step];
  const uint8_t fraction = static_cast<uint8_t>(
      (static_cast<uint16_t>(within_lap) * 8u) / duration);
  return {travel_step, thinkingPathIndex(travel_step, reverse), lap_modulo,
          fraction, reverse};
}

void addThinkingPathPixel(uint8_t frame[kPixelCount],
                          const int16_t travel_step, const bool reverse,
                          const uint8_t level) {
  const ThinkingPoint point =
      kThinkingPath[thinkingPathIndex(travel_step, reverse)];
  addPixelSaturating(frame, point.x, point.y, level);
}

uint8_t circularThinkingDistance(const uint8_t from, const uint8_t to) {
  const uint8_t direct = from > to ? static_cast<uint8_t>(from - to)
                                   : static_cast<uint8_t>(to - from);
  const uint8_t wrapped =
      static_cast<uint8_t>(kThinkingPathPointCount - direct);
  return direct < wrapped ? direct : wrapped;
}

uint8_t thinkingPeakLevel(const uint8_t brightness) {
  const uint8_t configured = highLevel(brightness);
  if (configured == 0) {
    return 0;
  }
  const uint8_t lifted = static_cast<uint8_t>(configured + 2u);
  return lifted > 6 ? 6 : lifted;
}

uint8_t thinkingLevel(const uint8_t brightness, const uint8_t nominal_level) {
  const uint8_t peak = thinkingPeakLevel(brightness);
  if (peak == 0 || nominal_level == 0) {
    return 0;
  }
  const uint8_t scaled = static_cast<uint8_t>(
      (static_cast<uint16_t>(peak) * nominal_level + 3u) / 7u);
  return scaled == 0 ? 1 : scaled;
}

uint32_t mixThinkingBits(uint32_t value) {
  value ^= value >> 16;
  value *= 0x7FEB352Du;
  value ^= value >> 15;
  value *= 0x846CA68Bu;
  value ^= value >> 16;
  return value;
}

struct Bubble {
  bool active;
  bool spawning;
  bool popping;
  int8_t x;
  int8_t y;
  int8_t drift;
  uint8_t brightness;
  uint16_t age_ms;
  uint16_t lifetime_ms;
  uint32_t signature;
};

constexpr uint16_t kBubbleCycleMs = 3500;
constexpr uint16_t kBubblePopWindowMs = kThinkingFrameIntervalMs;
constexpr uint16_t kBubbleSpawnOffsetsMs[kThinkingBubbleCapacity] = {
    0, 320, 760, 1240, 1790, 2380, 2940,
};

Bubble thinkingBubble(const uint8_t slot, const uint32_t elapsed_ms,
                      const uint8_t brightness) {
  const uint16_t spawn_offset = kBubbleSpawnOffsetsMs[slot];
  const uint32_t shifted =
      elapsed_ms + static_cast<uint32_t>(kBubbleCycleMs - spawn_offset);
  const uint32_t generation = shifted / kBubbleCycleMs;
  const uint16_t age_ms = static_cast<uint16_t>(shifted % kBubbleCycleMs);
  const uint32_t seed = mixThinkingBits(
      0xB7E15163u ^ static_cast<uint32_t>(slot) * 0x9E3779B9u ^
      generation * 0x85EBCA6Bu);
  const uint16_t lifetime_ms =
      static_cast<uint16_t>(2100u + seed % 701u);
  const bool active = age_ms < lifetime_ms;
  const bool spawning = age_ms < kThinkingFrameIntervalMs;
  const bool pop_enabled = ((seed >> 24) & 3u) == 0u;
  const bool popping = pop_enabled && age_ms >= lifetime_ms &&
                       age_ms < lifetime_ms + kBubblePopWindowMs;

  const int8_t start_y = static_cast<int8_t>(6 + ((seed >> 8) & 1u));
  uint8_t rise = static_cast<uint8_t>(
      (static_cast<uint32_t>(age_ms) *
       static_cast<uint8_t>(start_y + 1)) /
      lifetime_ms);
  if (rise > static_cast<uint8_t>(start_y)) {
    rise = static_cast<uint8_t>(start_y);
  }
  const int8_t y = static_cast<int8_t>(start_y - rise);

  const uint16_t wobble_period =
      static_cast<uint16_t>(280u + ((seed >> 12) % 140u));
  const uint8_t wobble_phase = static_cast<uint8_t>(
      (age_ms / wobble_period + ((seed >> 18) & 7u)) % 6u);
  const int8_t drift = wobble_phase == 2 ? 1 : (wobble_phase == 5 ? -1 : 0);
  const int8_t base_x = static_cast<int8_t>(seed % kMatrixWidth);
  int8_t x = static_cast<int8_t>(base_x + drift);
  if (x < 0) {
    x = 0;
  } else if (x >= kMatrixWidth) {
    x = kMatrixWidth - 1;
  }

  uint8_t nominal = 1;
  if (age_ms >= lifetime_ms / 4u &&
      age_ms < static_cast<uint16_t>((lifetime_ms * 3u) / 4u)) {
    nominal = 2;
    if (seed % 9u == 0u) {
      nominal = 3;
    }
  }
  const uint8_t bubble_brightness =
      highLevel(brightness) < nominal ? highLevel(brightness) : nominal;
  const uint32_t signature =
      mixThinkingBits(seed ^ static_cast<uint32_t>(age_ms) * 0x27D4EB2Du ^
                      static_cast<uint32_t>(static_cast<uint8_t>(x)) << 8 ^
                      static_cast<uint32_t>(static_cast<uint8_t>(y)));
  return {active,         spawning, popping, x,     y,
          drift,          bubble_brightness, age_ms, lifetime_ms,
          signature};
}

void populateThinkingBubbles(Bubble bubbles[kThinkingBubbleCapacity],
                             const uint32_t elapsed_ms,
                             const uint8_t brightness) {
  for (uint8_t slot = 0; slot < kThinkingBubbleCapacity; ++slot) {
    bubbles[slot] = thinkingBubble(slot, elapsed_ms, brightness);
  }
}

void renderThinkingBubbles(uint8_t frame[kPixelCount],
                           const uint32_t elapsed_ms,
                           const uint8_t brightness) {
  Bubble bubbles[kThinkingBubbleCapacity] = {};
  populateThinkingBubbles(bubbles, elapsed_ms, brightness);
  for (uint8_t slot = 0; slot < kThinkingBubbleCapacity; ++slot) {
    const Bubble& bubble = bubbles[slot];
    if (bubble.active) {
      addPixelSaturating(frame, bubble.x, bubble.y, bubble.brightness);
    }
    if (bubble.popping && bubble.brightness != 0) {
      // A one-frame surface fizz: three dim neighbours, never an explosion.
      addPixelSaturating(frame, bubble.x - 1, 0, 1);
      addPixelSaturating(frame, bubble.x + 1, 0, 1);
      addPixelSaturating(frame, bubble.x, 1, 1);
    }
  }
}

void renderThinking(uint8_t frame[kPixelCount], const uint32_t elapsed_ms,
                    const uint8_t brightness) {
  if (highLevel(brightness) == 0) {
    return;
  }

  const ThinkingPhase phase = thinkingPhase(elapsed_ms);
  const ThinkingPoint head = kThinkingPath[phase.path_index];

  // The independent ambient layer is intentionally slower and dimmer than
  // the comet. Collisions later add and saturate, creating a brief organic
  // brightening without any collision bookkeeping.
  renderThinkingBubbles(frame, elapsed_ms, brightness);

  // Four visible tail samples open through the crossing and tighten around
  // each lobe. At production brightness 3 these resolve to 4/2/1/1 behind a
  // level-5 head.
  const bool near_crossing = head.x >= 4 && head.x <= 8;
  const uint8_t trail_1 = 1;
  const uint8_t trail_2 = 2;
  const uint8_t trail_3 = near_crossing ? 4 : 3;
  const uint8_t trail_4 = near_crossing ? 7 : 5;
  addThinkingPathPixel(frame, phase.travel_step - trail_4, phase.reverse,
                       thinkingLevel(brightness, 1));
  addThinkingPathPixel(frame, phase.travel_step - trail_3, phase.reverse,
                       thinkingLevel(brightness, 2));
  addThinkingPathPixel(frame, phase.travel_step - trail_2, phase.reverse,
                       thinkingLevel(brightness, 3));
  addThinkingPathPixel(frame, phase.travel_step - trail_1, phase.reverse,
                       thinkingLevel(brightness, 6));
  addPixelSaturating(frame, head.x, head.y, thinkingLevel(brightness, 7));

  // A dim leading interpolation point softens the discrete grid.
  if (phase.step_fraction >= 6) {
    addThinkingPathPixel(frame, phase.travel_step + 1, phase.reverse,
                         thinkingLevel(brightness, 1));
  }

  // Sparse residual samples persist within the recent 15 path points. They
  // travel with the comet, so a viewer reconstructs both lobes over 1-2 s,
  // while the infinity outline is never statically illuminated.
  const uint8_t residual_shift = static_cast<uint8_t>(phase.lap_modulo & 1u);
  addThinkingPathPixel(frame, phase.travel_step - 9 - residual_shift,
                       phase.reverse, thinkingLevel(brightness, 1));
  addThinkingPathPixel(frame, phase.travel_step - 12, phase.reverse,
                       thinkingLevel(brightness, 1));
  addThinkingPathPixel(frame, phase.travel_step - 15 + residual_shift,
                       phase.reverse, thinkingLevel(brightness, 1));

  // At either physical crossing, a restrained centre pulse briefly excites
  // nearby pixels and immediately decays. The pulse alternates slightly in
  // strength from lap to lap, but never becomes a full-frame flash.
  uint8_t crossing_distance = circularThinkingDistance(phase.path_index, 0);
  const uint8_t second_crossing =
      circularThinkingDistance(phase.path_index,
                               kThinkingPathPointCount / 2);
  if (second_crossing < crossing_distance) {
    crossing_distance = second_crossing;
  }
  if (crossing_distance == 0) {
    setPixel(frame, 6, 3, thinkingLevel(brightness, 7));
    setPixel(frame, 6, 4, thinkingLevel(brightness, 6));
    setPixel(frame, 5, 3, thinkingLevel(brightness, 2));
    setPixel(frame, 7, 4, thinkingLevel(brightness, 2));
  } else if (crossing_distance == 1) {
    setPixel(frame, 6, 4, thinkingLevel(brightness, 6));
    setPixel(frame, 5, 3, thinkingLevel(brightness, 2));
    setPixel(frame, 7, 3, thinkingLevel(brightness, 1));
    setPixel(frame, 5, 4, thinkingLevel(brightness, 1));
    setPixel(frame, 7, 4, thinkingLevel(brightness, 2));
  }
}

void renderReading(uint8_t frame[kPixelCount], const uint32_t elapsed_ms,
                   const uint8_t brightness) {
  const int16_t scan = static_cast<int16_t>((elapsed_ms / 100) % kMatrixWidth);
  for (uint8_t y = 0; y < kMatrixHeight; ++y) {
    setPixel(frame, scan, y, highLevel(brightness));
    setPixel(frame, scan - 1, y, lowLevel(brightness));
  }
}

void renderWriting(uint8_t frame[kPixelCount], const uint32_t elapsed_ms,
                   const uint8_t brightness) {
  constexpr uint8_t kWritingRows = 4;
  constexpr uint8_t kPathLength = kMatrixWidth * kWritingRows;
  const uint8_t progress = static_cast<uint8_t>((elapsed_ms / 85) % kPathLength);
  for (uint8_t i = 0; i <= progress; ++i) {
    const uint8_t line = static_cast<uint8_t>(i / kMatrixWidth);
    const uint8_t offset = static_cast<uint8_t>(i % kMatrixWidth);
    const int16_t x = (line & 1u) == 0 ? offset : kMatrixWidth - 1 - offset;
    const int16_t y = static_cast<int16_t>(line * 2);
    setPixel(frame, x, y,
             i == progress ? highLevel(brightness) : lowLevel(brightness));
    if (i == progress) {
      setPixel(frame, x, y + 1, mediumLevel(brightness));
    }
  }
}

void renderCommand(uint8_t frame[kPixelCount], const uint32_t elapsed_ms,
                   const uint8_t brightness) {
  const int16_t head = static_cast<int16_t>((elapsed_ms / 100) % 18) - 2;
  for (int16_t x = head - 4; x < head; ++x) {
    setPixel(frame, x, 4, mediumLevel(brightness));
  }
  setPixel(frame, head, 4, highLevel(brightness));
  setPixel(frame, head - 1, 3, highLevel(brightness));
  setPixel(frame, head - 1, 5, highLevel(brightness));
  setPixel(frame, head - 2, 2, lowLevel(brightness));
  setPixel(frame, head - 2, 6, lowLevel(brightness));
}

void renderBuilding(uint8_t frame[kPixelCount], const uint32_t elapsed_ms,
                    const uint8_t brightness) {
  constexpr uint8_t kBlocksPerRow = 7;
  constexpr uint8_t kRows = 4;
  constexpr uint8_t kBlockCount = kBlocksPerRow * kRows;
  constexpr uint8_t kHoldSteps = 5;
  uint8_t progress =
      static_cast<uint8_t>((elapsed_ms / 170) % (kBlockCount + kHoldSteps));
  if (progress > kBlockCount) {
    progress = kBlockCount;
  }
  for (uint8_t block = 0; block < progress; ++block) {
    const uint8_t row = static_cast<uint8_t>(block / kBlocksPerRow);
    const uint8_t column = static_cast<uint8_t>(block % kBlocksPerRow);
    const int16_t x = static_cast<int16_t>(column * 2) - (row & 1u);
    const int16_t y = static_cast<int16_t>(kMatrixHeight - 1 - row);
    const uint8_t level = block + 1 == progress ? highLevel(brightness)
                                                : mediumLevel(brightness);
    setPixel(frame, x, y, level);
    setPixel(frame, x + 1, y, level);
  }
}

void renderTesting(uint8_t frame[kPixelCount], const uint32_t elapsed_ms,
                   const uint8_t brightness) {
  const uint8_t progress =
      pingPongPosition(elapsed_ms, 100, kMatrixWidth);
  for (uint8_t x = 0; x <= progress; ++x) {
    setPixel(frame, x, 3, mediumLevel(brightness));
    setPixel(frame, x, 4, mediumLevel(brightness));
  }
  setPixel(frame, progress, 3, highLevel(brightness));
  setPixel(frame, progress, 4, highLevel(brightness));
  setPixel(frame, 0, 2, lowLevel(brightness));
  setPixel(frame, 0, 5, lowLevel(brightness));
  setPixel(frame, 12, 2, lowLevel(brightness));
  setPixel(frame, 12, 5, lowLevel(brightness));
}

void renderFlashing(uint8_t frame[kPixelCount], const uint32_t elapsed_ms,
                    const uint8_t brightness) {
  const uint8_t step = static_cast<uint8_t>((elapsed_ms / 90) % 12);
  for (uint8_t x = 0; x < kMatrixWidth; x += 2) {
    const int16_t y = static_cast<int16_t>((step + x * 5u) % 12u) - 4;
    setPixel(frame, x, y, highLevel(brightness));
    setPixel(frame, x, y - 1, mediumLevel(brightness));
    setPixel(frame, x, y - 2, lowLevel(brightness));
  }
}

void renderWaiting(uint8_t frame[kPixelCount], const uint32_t elapsed_ms,
                   const uint8_t brightness) {
  const bool bright = ((elapsed_ms / 650) & 1u) == 0;
  drawGlyph(frame, frames::kQuestionMark,
            bright ? highLevel(brightness) : lowLevel(brightness));
}

void renderSuccess(uint8_t frame[kPixelCount], const uint32_t elapsed_ms,
                   const uint8_t brightness) {
  uint8_t level = mediumLevel(brightness);
  if (elapsed_ms < 800) {
    const bool bright = ((elapsed_ms / 200) & 1u) == 0;
    level = bright ? highLevel(brightness) : 0;
  }
  drawGlyph(frame, frames::kCheckMark, level);
}

void renderError(uint8_t frame[kPixelCount], const uint32_t elapsed_ms,
                 const uint8_t brightness) {
  uint8_t level = lowLevel(brightness);
  if (elapsed_ms < 900) {
    const bool bright = ((elapsed_ms / 150) & 1u) == 0;
    level = bright ? highLevel(brightness) : 0;
  }
  drawGlyph(frame, frames::kErrorX, level);
}

void renderOffline(uint8_t frame[kPixelCount], const uint32_t elapsed_ms,
                   const uint8_t brightness) {
  const bool bright = ((elapsed_ms / 750) & 1u) == 0;
  uint8_t limited = highLevel(brightness);
  if (limited > 3) {
    limited = 3;
  }
  drawGlyph(frame, frames::kOffline,
            bright ? limited : lowLevel(brightness));
}

void renderSubagent(uint8_t frame[kPixelCount], const uint32_t elapsed_ms,
                    const uint8_t brightness) {
  const uint8_t first = static_cast<uint8_t>((elapsed_ms / 90) % kMatrixWidth);
  const uint8_t second = static_cast<uint8_t>(
      kMatrixWidth - 1 - ((elapsed_ms / 130) % kMatrixWidth));
  const uint8_t third = pingPongPosition(elapsed_ms, 160, kMatrixWidth);
  setPixel(frame, first, 2, highLevel(brightness));
  setPixel(frame, second, 5, mediumLevel(brightness));
  setPixel(frame, third, 7, lowLevel(brightness));
}

void addActiveCount(uint8_t frame[kPixelCount], const uint8_t active_count,
                    const uint8_t brightness) {
  uint8_t dots = active_count;
  if (dots > kMaxActiveCountDots) {
    dots = kMaxActiveCountDots;
  }
  for (uint8_t i = 0; i < dots; ++i) {
    setPixel(frame, static_cast<int16_t>(kMatrixWidth - 1 - i), 0,
             lowLevel(brightness));
  }
}

}  // namespace

#if defined(UNOQ_CODEX_MATRIX_HOST_TEST)
ThinkingDebugSnapshot thinkingDebugSnapshot(const uint32_t elapsed_ms) {
  const ThinkingPhase phase = thinkingPhase(elapsed_ms);
  const ThinkingPoint head = kThinkingPath[phase.path_index];
  Bubble bubbles[kThinkingBubbleCapacity] = {};
  populateThinkingBubbles(bubbles, elapsed_ms, kMaxBrightness);

  uint8_t active_bubbles = 0;
  uint8_t active_mask = 0;
  uint8_t spawn_mask = 0;
  uint8_t pop_mask = 0;
  uint32_t signature = 0x243F6A88u;
  for (uint8_t slot = 0; slot < kThinkingBubbleCapacity; ++slot) {
    const uint8_t bit = static_cast<uint8_t>(1u << slot);
    if (bubbles[slot].active) {
      ++active_bubbles;
      active_mask = static_cast<uint8_t>(active_mask | bit);
    }
    if (bubbles[slot].spawning) {
      spawn_mask = static_cast<uint8_t>(spawn_mask | bit);
    }
    if (bubbles[slot].popping) {
      pop_mask = static_cast<uint8_t>(pop_mask | bit);
    }
    signature = mixThinkingBits(
        signature ^ bubbles[slot].signature ^
        static_cast<uint32_t>(bubbles[slot].age_ms) << (slot & 7u) ^
        static_cast<uint32_t>(bubbles[slot].lifetime_ms) ^
        static_cast<uint32_t>(static_cast<int16_t>(bubbles[slot].drift) + 1)
            << (slot + 8u));
  }
  return {static_cast<uint8_t>(head.x), static_cast<uint8_t>(head.y),
          active_bubbles, active_mask, spawn_mask, pop_mask, signature,
          phase.reverse};
}
#endif

void renderAnimation(const StateId state, const uint32_t now_ms,
                     const uint32_t state_entered_ms, const uint8_t brightness,
                     const uint8_t active_count, const bool show_active_count,
                     uint8_t frame[kPixelCount]) {
  memset(frame, 0, kPixelCount);
  const uint32_t elapsed_ms = now_ms - state_entered_ms;

  switch (state) {
    case OFF:
      break;
    case IDLE:
      renderIdle(frame, elapsed_ms, brightness);
      break;
    case THINKING:
      renderThinking(frame, elapsed_ms, brightness);
      break;
    case READING:
      renderReading(frame, elapsed_ms, brightness);
      break;
    case WRITING:
      renderWriting(frame, elapsed_ms, brightness);
      break;
    case COMMAND:
      renderCommand(frame, elapsed_ms, brightness);
      break;
    case BUILDING:
      renderBuilding(frame, elapsed_ms, brightness);
      break;
    case TESTING:
      renderTesting(frame, elapsed_ms, brightness);
      break;
    case FLASHING:
      renderFlashing(frame, elapsed_ms, brightness);
      break;
    case WAITING:
      renderWaiting(frame, elapsed_ms, brightness);
      break;
    case SUCCESS:
      renderSuccess(frame, elapsed_ms, brightness);
      break;
    case ERROR:
      renderError(frame, elapsed_ms, brightness);
      break;
    case OFFLINE:
      renderOffline(frame, elapsed_ms, brightness);
      break;
    case SUBAGENT:
      renderSubagent(frame, elapsed_ms, brightness);
      break;
    default:
      renderOffline(frame, elapsed_ms, brightness);
      break;
  }

  // OFF must remain completely dark. OFFLINE uses its own unambiguous glyph
  // and deliberately omits potentially stale session-count dots.
  if (show_active_count && state != OFF && state != OFFLINE) {
    addActiveCount(frame, active_count, brightness);
  }
}

}  // namespace unoq_codex_matrix
