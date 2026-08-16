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

bool pingPongMovingForward(const uint32_t elapsed_ms, const uint16_t step_ms,
                           const uint8_t extent) {
  const uint16_t leg = static_cast<uint16_t>(extent - 1);
  const uint16_t cycle = static_cast<uint16_t>(leg * 2);
  return ((elapsed_ms / step_ms) % cycle) <= leg;
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

void renderThinking(uint8_t frame[kPixelCount], const uint32_t elapsed_ms,
                    const uint8_t brightness) {
  constexpr uint16_t kStepMs = 90;
  const int16_t head = pingPongPosition(elapsed_ms, kStepMs, kMatrixWidth);
  const int16_t direction =
      pingPongMovingForward(elapsed_ms, kStepMs, kMatrixWidth) ? 1 : -1;
  setPixel(frame, head, 3, highLevel(brightness));
  setPixel(frame, head - direction, 3, mediumLevel(brightness));
  setPixel(frame, head - 2 * direction, 3, lowLevel(brightness));
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
