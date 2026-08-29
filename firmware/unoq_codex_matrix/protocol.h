#ifndef UNOQ_CODEX_MATRIX_PROTOCOL_H
#define UNOQ_CODEX_MATRIX_PROTOCOL_H

#include <stdint.h>

namespace unoq_codex_matrix {

// These IDs are part of the public Linux/MCU protocol. Never reorder them.
enum StateId : uint8_t {
  OFF = 0,
  IDLE = 1,
  THINKING = 2,
  READING = 3,
  WRITING = 4,
  COMMAND = 5,
  BUILDING = 6,
  TESTING = 7,
  FLASHING = 8,
  WAITING = 9,
  SUCCESS = 10,
  ERROR = 11,
  OFFLINE = 12,
  SUBAGENT = 13,
};

constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kFirmwareMajor = 0;
constexpr uint8_t kFirmwareMinor = 2;
constexpr uint8_t kFirmwarePatch = 0;

constexpr uint8_t kMatrixWidth = 13;
constexpr uint8_t kMatrixHeight = 8;
constexpr uint16_t kPixelCount = kMatrixWidth * kMatrixHeight;

// The matrix supports values 0..7. Limiting normal operation to 5 avoids
// continuously driving the LEDs at their maximum level.
constexpr uint8_t kMaxBrightness = 5;
constexpr uint8_t kMaxActiveCountDots = 3;
constexpr uint8_t kMaxQuotaPercent = 100;
constexpr uint16_t kDefaultFrameIntervalMs = 100;
constexpr uint16_t kMinFrameIntervalMs = 50;
constexpr uint16_t kMaxFrameIntervalMs = 150;
constexpr uint32_t kDefaultOfflineTimeoutMs = 12000UL;
constexpr uint32_t kMinOfflineTimeoutMs = 1000UL;
constexpr uint32_t kMaxOfflineTimeoutMs = 600000UL;

static_assert(kPixelCount == 104, "UNO Q matrix must contain 104 pixels");
static_assert(static_cast<uint8_t>(SUBAGENT) == 13,
              "state wire IDs must remain 0 through 13");
static_assert(kMaxBrightness <= 7,
              "three-bit grayscale cannot represent this brightness");
static_assert(kMaxOfflineTimeoutMs < 0x80000000UL,
              "wrap-safe elapsed-time comparisons require a short timeout");

enum RpcResult : int32_t {
  RPC_REJECTED = 0,
  // Boolean-style mutating RPCs return 1 on acceptance. Brightness is the
  // exception: it returns the accepted level itself (0..kMaxBrightness).
  RPC_OK = 1,
};

inline bool isValidState(const int32_t state) {
  return state >= static_cast<int32_t>(OFF) &&
         state <= static_cast<int32_t>(SUBAGENT);
}

// get_status byte layout, most-significant byte first:
// [protocol version][displayed state][active session count][brightness]
inline uint32_t packStatus(const StateId state, const uint8_t active_count,
                           const uint8_t brightness) {
  return (static_cast<uint32_t>(kProtocolVersion) << 24) |
         (static_cast<uint32_t>(state) << 16) |
         (static_cast<uint32_t>(active_count) << 8) |
         static_cast<uint32_t>(brightness);
}

// get_version byte layout, most-significant byte first:
// [protocol version][firmware major][firmware minor][firmware patch]
inline uint32_t packVersion() {
  return (static_cast<uint32_t>(kProtocolVersion) << 24) |
         (static_cast<uint32_t>(kFirmwareMajor) << 16) |
         (static_cast<uint32_t>(kFirmwareMinor) << 8) |
         static_cast<uint32_t>(kFirmwarePatch);
}

}  // namespace unoq_codex_matrix

#endif  // UNOQ_CODEX_MATRIX_PROTOCOL_H
