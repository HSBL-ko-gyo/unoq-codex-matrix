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

void addIntensityQ8(uint8_t frame[kPixelCount], int16_t x, int16_t y,
                    uint16_t intensity_q8);

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

uint8_t idleFadeOpacity(const uint32_t elapsed_ms) {
  if (elapsed_ms >= kIdleFadeInMs) {
    return 255;
  }
  return static_cast<uint8_t>(
      (static_cast<uint32_t>(elapsed_ms) * 255u) / kIdleFadeInMs);
}

void renderIdle(uint8_t frame[kPixelCount], const uint32_t elapsed_ms,
                const uint8_t brightness) {
  (void)elapsed_ms;
  const uint8_t configured = highLevel(brightness);
  if (configured == 0) {
    return;
  }
  // READY remains completely static after the bounded entry fade.
  constexpr uint8_t kReadyLevel = 1;
  setPixel(frame, 4, 4, kReadyLevel);
  setPixel(frame, 6, 4, kReadyLevel);
  setPixel(frame, 8, 4, kReadyLevel);
}

struct ThinkingPoint {
  int8_t x;
  int8_t y;
};

// Forty integer anchors describe a balanced inner lemniscate. Rendering uses
// Q8 interpolation between them, so the array is not a frame list.
constexpr ThinkingPoint kThinkingPath[kThinkingPathPointCount] = {
    {6, 4},  {6, 3},  {5, 3},  {5, 2},  {4, 2},
    {4, 1},  {3, 1},  {2, 1},  {2, 2},  {1, 2},
    {1, 3},  {1, 4},  {1, 5},  {2, 5},  {2, 6},
    {3, 6},  {4, 6},  {4, 5},  {5, 5},  {5, 4},
    {6, 4},  {6, 3},  {7, 3},  {7, 2},  {8, 2},
    {8, 1},  {9, 1},  {10, 1}, {10, 2}, {11, 2},
    {11, 3}, {11, 4}, {11, 5}, {10, 5}, {10, 6},
    {9, 6},  {8, 6},  {8, 5},  {7, 5},  {7, 4},
};

// Every path segment takes the same time: perceived motion stays constant.
constexpr uint8_t kThinkingStepDurationMs[kThinkingPathPointCount] = {
    105, 105, 105, 105, 105, 105, 105, 105,
    105, 105, 105, 105, 105, 105, 105, 105,
    105, 105, 105, 105, 105, 105, 105, 105,
    105, 105, 105, 105, 105, 105, 105, 105,
    105, 105, 105, 105, 105, 105, 105, 105,
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
  uint8_t fraction_q8;
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
  return reverse && wrapped != 0
             ? static_cast<uint8_t>(kThinkingPathPointCount - wrapped)
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
      (static_cast<uint32_t>(within_lap) * 256u) / duration);
  return {travel_step, thinkingPathIndex(travel_step, reverse), lap_modulo,
          fraction, reverse};
}

struct SubpixelPoint {
  uint16_t x_q8;
  uint16_t y_q8;
};

SubpixelPoint thinkingCoordinate(const int16_t travel_step,
                                 const bool reverse,
                                 const uint8_t fraction_q8) {
  const ThinkingPoint from =
      kThinkingPath[thinkingPathIndex(travel_step, reverse)];
  const ThinkingPoint to =
      kThinkingPath[thinkingPathIndex(travel_step + 1, reverse)];
  const int16_t x_q8 = static_cast<int16_t>(from.x * 256) +
                       static_cast<int16_t>((to.x - from.x) * fraction_q8);
  const int16_t y_q8 = static_cast<int16_t>(from.y * 256) +
                       static_cast<int16_t>((to.y - from.y) * fraction_q8);
  return {static_cast<uint16_t>(x_q8), static_cast<uint16_t>(y_q8)};
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

void addIntensityQ8(uint8_t frame[kPixelCount], const int16_t x,
                    const int16_t y, const uint16_t intensity_q8) {
  // Round fractional spatial contributions once; do not add temporal dither.
  addPixelSaturating(frame, x, y,
                     static_cast<uint8_t>((intensity_q8 + 128u) >> 8));
}

void addSubpixelParticle(uint8_t frame[kPixelCount], const uint16_t x_q8,
                         const uint16_t y_q8,
                         const uint16_t intensity_q8) {
  const int16_t x = static_cast<int16_t>(x_q8 >> 8);
  const int16_t y = static_cast<int16_t>(y_q8 >> 8);
  const uint16_t fx = static_cast<uint16_t>(x_q8 & 0xFFu);
  const uint16_t fy = static_cast<uint16_t>(y_q8 & 0xFFu);
  const uint16_t inverse_x = static_cast<uint16_t>(256u - fx);
  const uint16_t inverse_y = static_cast<uint16_t>(256u - fy);
  const uint32_t weights[4] = {
      static_cast<uint32_t>(inverse_x) * inverse_y,
      static_cast<uint32_t>(fx) * inverse_y,
      static_cast<uint32_t>(inverse_x) * fy,
      static_cast<uint32_t>(fx) * fy,
  };
  const int8_t dx[4] = {0, 1, 0, 1};
  const int8_t dy[4] = {0, 0, 1, 1};
  for (uint8_t index = 0; index < 4; ++index) {
    const uint16_t contribution = static_cast<uint16_t>(
        (static_cast<uint32_t>(intensity_q8) * weights[index]) >> 16);
    if (contribution == 0) {
      continue;
    }
    addIntensityQ8(frame, x + dx[index], y + dy[index], contribution);
  }
}

uint16_t intensityWithOpacity(const uint8_t level, const uint8_t opacity_q8) {
  return static_cast<uint16_t>(
      (static_cast<uint32_t>(level) * 256u * opacity_q8 + 127u) / 255u);
}

void addThinkingParticle(uint8_t frame[kPixelCount],
                         const ThinkingPhase& phase,
                         const int16_t step_offset, const uint16_t intensity_q8,
                         const uint8_t temporal_phase) {
  const SubpixelPoint point =
      thinkingCoordinate(phase.travel_step + step_offset, phase.reverse,
                         phase.fraction_q8);
  (void)temporal_phase;
  addSubpixelParticle(frame, point.x_q8, point.y_q8, intensity_q8);
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
  uint16_t x_q8;
  uint16_t y_q8;
  uint8_t brightness;
  uint8_t opacity_q8;
  uint16_t age_ms;
  uint16_t lifetime_ms;
  uint16_t fade_in_ms;
  uint16_t fade_out_ms;
  uint32_t signature;
};

constexpr uint16_t kBubbleCycleMs = 4200;
constexpr uint16_t kBubbleSpawnOffsetsMs[kThinkingBubbleCapacity] = {
    0, 520, 1080, 1690, 2350, 3070, 3720,
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
  // Cross the 6-7 row rise quickly enough that deterministic 3-bit spatial
  // rounding does not leave a bubble parked on one LED for too long.
  const uint16_t lifetime_ms =
      static_cast<uint16_t>(1550u + seed % 401u);
  const uint16_t fade_in_ms =
      static_cast<uint16_t>(250u + ((seed >> 5) % 151u));
  const uint16_t fade_out_ms =
      static_cast<uint16_t>(200u + ((seed >> 13) % 151u));
  const bool active = age_ms < lifetime_ms;
  const bool spawning = age_ms < fade_in_ms;

  uint8_t opacity_q8 = 0;
  if (active) {
    if (age_ms < fade_in_ms) {
      opacity_q8 = static_cast<uint8_t>(
          (static_cast<uint32_t>(age_ms) * 255u) / fade_in_ms);
    } else if (age_ms > lifetime_ms - fade_out_ms) {
      opacity_q8 = static_cast<uint8_t>(
          (static_cast<uint32_t>(lifetime_ms - age_ms) * 255u) /
          fade_out_ms);
    } else {
      opacity_q8 = 255;
    }
  }

  const uint16_t start_y_q8 = static_cast<uint16_t>(
      static_cast<uint16_t>(6u + ((seed >> 8) & 1u)) * 256u);
  const uint16_t bounded_age = age_ms < lifetime_ms ? age_ms : lifetime_ms;
  const uint16_t y_q8 = static_cast<uint16_t>(
      (static_cast<uint32_t>(start_y_q8) * (lifetime_ms - bounded_age)) /
      lifetime_ms);

  // The horizontal coordinate is an integer chosen once per generation. The
  // only motion is the continuous Q8 rise toward the surface.
  const uint16_t x_q8 =
      static_cast<uint16_t>((seed % kMatrixWidth) * 256u);
  const uint8_t nominal = ((seed >> 24) % 7u) == 0u ? 2u : 1u;
  const uint8_t bubble_brightness =
      highLevel(brightness) < nominal ? highLevel(brightness) : nominal;
  const uint32_t signature =
      mixThinkingBits(seed ^ static_cast<uint32_t>(age_ms) * 0x27D4EB2Du ^
                      static_cast<uint32_t>(static_cast<uint16_t>(x_q8)) << 8 ^
                      static_cast<uint32_t>(y_q8) ^ opacity_q8);
  return {active,
          spawning,
          x_q8,
          y_q8,
          bubble_brightness,
          opacity_q8,
          age_ms,
          lifetime_ms,
          fade_in_ms,
          fade_out_ms,
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
      addSubpixelParticle(
          frame, bubble.x_q8, bubble.y_q8,
          intensityWithOpacity(bubble.brightness, bubble.opacity_q8));
    }
  }
}

uint8_t thinkingFadeOpacity(const uint32_t elapsed_ms) {
  if (elapsed_ms >= kThinkingFadeInMs) {
    return 255;
  }
  return static_cast<uint8_t>(
      (static_cast<uint32_t>(elapsed_ms) * 255u) / kThinkingFadeInMs);
}

uint16_t circularDistanceQ8(const uint16_t from, const uint16_t to,
                            const uint16_t circumference) {
  const uint16_t direct = from > to ? static_cast<uint16_t>(from - to)
                                    : static_cast<uint16_t>(to - from);
  const uint16_t wrapped = static_cast<uint16_t>(circumference - direct);
  return direct < wrapped ? direct : wrapped;
}

uint8_t thinkingCenterOpacity(const ThinkingPhase& phase) {
  constexpr uint16_t kPathQ8 = kThinkingPathPointCount * 256u;
  int32_t position = static_cast<int32_t>(phase.path_index) * 256;
  position += phase.reverse ? -phase.fraction_q8 : phase.fraction_q8;
  while (position < 0) {
    position += kPathQ8;
  }
  while (position >= kPathQ8) {
    position -= kPathQ8;
  }
  uint16_t distance = circularDistanceQ8(
      static_cast<uint16_t>(position), 0, kPathQ8);
  const uint16_t second = circularDistanceQ8(
      static_cast<uint16_t>(position), kPathQ8 / 2u, kPathQ8);
  if (second < distance) {
    distance = second;
  }
  constexpr uint16_t kEnvelopeSpanQ8 = 320;
  if (distance >= kEnvelopeSpanQ8) {
    return 0;
  }
  return static_cast<uint8_t>(
      (static_cast<uint32_t>(kEnvelopeSpanQ8 - distance) * 255u) /
      kEnvelopeSpanQ8);
}

uint8_t residualOpacity(const uint8_t fraction_q8, const uint8_t salt) {
  const uint8_t phase = static_cast<uint8_t>(fraction_q8 + salt);
  const uint8_t triangle = phase < 128u ? phase : static_cast<uint8_t>(255u - phase);
  return static_cast<uint8_t>(96u + triangle / 2u);
}

void renderThinking(uint8_t frame[kPixelCount], const uint32_t elapsed_ms,
                    const uint8_t brightness) {
  if (highLevel(brightness) == 0) {
    return;
  }

  const ThinkingPhase phase = thinkingPhase(elapsed_ms);
  const uint8_t frame_phase =
      static_cast<uint8_t>(elapsed_ms / kThinkingFrameIntervalMs);

  renderThinkingBubbles(frame, elapsed_ms, brightness);

  // Every tail sample follows the same Q8 position interpolation as the head;
  // brightness decays with age instead of becoming a row of jumping dots.
  const uint8_t trail_levels[5] = {6, 4, 3, 2, 1};
  const int8_t trail_offsets[5] = {-1, -2, -4, -6, -8};
  for (uint8_t index = 0; index < 5; ++index) {
    addThinkingParticle(
        frame, phase, trail_offsets[index],
        static_cast<uint16_t>(thinkingLevel(brightness, trail_levels[index])) *
            256u,
        static_cast<uint8_t>(frame_phase + index * 31u));
  }

  // Residual energy is rounded once into the driver's native 3-bit input.
  const int8_t residual_offsets[3] = {-11, -14, -17};
  const uint8_t residual_salts[3] = {17, 103, 211};
  for (uint8_t index = 0; index < 3; ++index) {
    const uint8_t opacity =
        residualOpacity(phase.fraction_q8, residual_salts[index]);
    addThinkingParticle(
        frame, phase, residual_offsets[index],
        intensityWithOpacity(thinkingLevel(brightness, 1), opacity),
        static_cast<uint8_t>(frame_phase + residual_salts[index]));
  }

  addThinkingParticle(
      frame, phase, 0,
      static_cast<uint16_t>(thinkingLevel(brightness, 7)) * 256u,
      static_cast<uint8_t>(frame_phase + 7u));

  // A 250 ms-class triangular envelope replaces the previous on/off spark.
  const uint8_t center_opacity = thinkingCenterOpacity(phase);
  addIntensityQ8(frame, 6, 3,
                 intensityWithOpacity(thinkingLevel(brightness, 7),
                                      center_opacity));
  addIntensityQ8(frame, 6, 4,
                 intensityWithOpacity(thinkingLevel(brightness, 5),
                                      center_opacity));
  addIntensityQ8(frame, 5, 3,
                 intensityWithOpacity(thinkingLevel(brightness, 2),
                                      center_opacity));
  addIntensityQ8(frame, 7, 4,
                 intensityWithOpacity(thinkingLevel(brightness, 2),
                                      center_opacity));
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

void scaleFrameOpacity(uint8_t frame[kPixelCount], const uint8_t opacity_q8) {
  if (opacity_q8 == 255) {
    return;
  }
  for (uint16_t index = 0; index < kPixelCount; ++index) {
    const uint16_t product =
        static_cast<uint16_t>(frame[index]) * opacity_q8;
    frame[index] = static_cast<uint8_t>((product + 127u) / 255u);
  }
}

}  // namespace

#if defined(UNOQ_CODEX_MATRIX_HOST_TEST)
ThinkingDebugSnapshot thinkingDebugSnapshot(const uint32_t elapsed_ms) {
  const ThinkingPhase phase = thinkingPhase(elapsed_ms);
  const SubpixelPoint head = thinkingCoordinate(
      phase.travel_step, phase.reverse, phase.fraction_q8);
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
    signature = mixThinkingBits(
        signature ^ bubbles[slot].signature ^
        static_cast<uint32_t>(bubbles[slot].age_ms) << (slot & 7u) ^
        static_cast<uint32_t>(bubbles[slot].lifetime_ms) ^
        static_cast<uint32_t>(bubbles[slot].opacity_q8) << (slot + 8u));
  }
  return {head.x_q8,
          head.y_q8,
          active_bubbles,
          active_mask,
          spawn_mask,
          pop_mask,
          thinkingFadeOpacity(elapsed_ms),
          thinkingCenterOpacity(phase),
          bubbles[0].opacity_q8,
          bubbles[0].brightness,
          bubbles[0].x_q8,
          bubbles[0].y_q8,
          bubbles[0].lifetime_ms,
          bubbles[0].fade_in_ms,
          bubbles[0].fade_out_ms,
          signature,
          phase.reverse};
}

IdleDebugSnapshot idleDebugSnapshot(const uint32_t elapsed_ms) {
  return {idleFadeOpacity(elapsed_ms)};
}

void addThinkingTestParticle(const uint16_t x_q8, const uint16_t y_q8,
                             const uint16_t intensity_q8,
                             const uint8_t temporal_phase,
                             uint8_t frame[kPixelCount]) {
  (void)temporal_phase;
  addSubpixelParticle(frame, x_q8, y_q8, intensity_q8);
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
  if (state == THINKING) {
    scaleFrameOpacity(frame, thinkingFadeOpacity(elapsed_ms));
  } else if (state == IDLE) {
    scaleFrameOpacity(frame, idleFadeOpacity(elapsed_ms));
  }
}

void renderTransition(const StateId from, const StateId to,
                      const uint32_t now_ms,
                      const uint32_t from_state_entered_ms,
                      const uint32_t transition_started_ms,
                      const uint8_t brightness, const uint8_t active_count,
                      const bool show_active_count,
                      uint8_t frame[kPixelCount]) {
  const uint32_t elapsed_ms = now_ms - transition_started_ms;
  const uint16_t duration_ms = transitionDurationMs(from, to);
  if (!shouldCrossfadeThinkingTransition(from, to) ||
      elapsed_ms >= duration_ms) {
    const uint32_t target_now = from == IDLE && to == THINKING
                                    ? transition_started_ms + duration_ms
                                    : transition_started_ms;
    renderAnimation(to, target_now, transition_started_ms, brightness,
                    active_count, show_active_count, frame);
    return;
  }

  uint8_t from_frame[kPixelCount] = {};
  uint8_t to_frame[kPixelCount] = {};
  renderAnimation(from, now_ms, from_state_entered_ms, brightness, active_count,
                  show_active_count, from_frame);
  // THINKING advances through its own 420 ms fade while the quiet IDLE core
  // recedes. Exit transitions hold the target's first frame for 210 ms.
  const bool entering_thinking = from == IDLE && to == THINKING;
  const uint32_t target_now = entering_thinking ? now_ms : transition_started_ms;
  renderAnimation(to, target_now, transition_started_ms, brightness,
                  active_count, show_active_count, to_frame);
  const uint8_t to_opacity = static_cast<uint8_t>(
      (static_cast<uint32_t>(elapsed_ms) * 255u) / duration_ms);
  const uint8_t from_opacity = static_cast<uint8_t>(255u - to_opacity);
  for (uint16_t index = 0; index < kPixelCount; ++index) {
    const uint16_t from_level = static_cast<uint16_t>(
        (static_cast<uint16_t>(from_frame[index]) * from_opacity + 127u) /
        255u);
    const uint16_t target_level = entering_thinking
                                      ? to_frame[index]
                                      : static_cast<uint16_t>(
                                            (static_cast<uint16_t>(
                                                 to_frame[index]) *
                                                 to_opacity +
                                             127u) /
                                            255u);
    const uint16_t combined = from_level + target_level;
    frame[index] = combined > kMaxBrightness
                       ? kMaxBrightness
                       : static_cast<uint8_t>(combined);
  }
}

}  // namespace unoq_codex_matrix
