#include <Arduino.h>
#include <Arduino_LED_Matrix.h>
#include <Arduino_RouterBridge.h>

#include "animations.h"
#include "protocol.h"

using namespace unoq_codex_matrix;

namespace {

struct PendingUpdates {
  bool has_state;
  bool has_invalid_state;
  bool has_heartbeat;
  bool has_brightness;
  StateId state;
  uint8_t active_count;
  uint8_t brightness;
  uint16_t frame_interval_ms;
  uint32_t offline_timeout_ms;
  bool show_active_count;
};

Arduino_LED_Matrix matrix;
PendingUpdates pending = {};
uint8_t frame[kPixelCount] = {};

StateId requested_state = IDLE;
StateId displayed_state = OFFLINE;
uint8_t active_count = 0;
uint8_t brightness = 3;
uint16_t frame_interval_ms = kDefaultFrameIntervalMs;
uint32_t offline_timeout_ms = kDefaultOfflineTimeoutMs;
bool show_active_count = true;
bool have_requested_state = false;
bool have_heartbeat = false;
bool matrix_ready = false;
bool bridge_ready = false;
uint32_t last_heartbeat_ms = 0;
uint32_t state_entered_ms = 0;
uint32_t last_frame_ms = 0;
bool transition_active = false;
StateId transition_from_state = IDLE;
uint32_t transition_from_entered_ms = 0;
uint32_t transition_started_ms = 0;
uint32_t render_total_us = 0;
uint16_t render_sample_count = 0;
uint16_t render_max_us = 0;

int32_t setStateRpc(int32_t protocol_version, int32_t state,
                    int32_t sessions, int32_t requested_frame_interval_ms,
                    int32_t requested_offline_timeout_ms,
                    int32_t requested_show_count);
int32_t heartbeatRpc(int32_t protocol_version);
int32_t setBrightnessRpc(int32_t protocol_version, int32_t level);
uint32_t getStatusRpc();
uint32_t getVersionRpc();
uint32_t getRenderMetricsRpc();

uint16_t currentFrameIntervalMs() {
  return transition_active
             ? kThinkingFrameIntervalMs
             : effectiveFrameIntervalMs(displayed_state, frame_interval_ms);
}

void noteHeartbeat(const uint32_t now_ms) {
  last_heartbeat_ms = now_ms;
  have_heartbeat = true;
}

void enterState(const StateId state, const uint32_t now_ms) {
  if (displayed_state == state) {
    return;
  }
  const StateId previous_state = displayed_state;
  const uint32_t previous_entered_ms = state_entered_ms;
  transition_active =
      shouldCrossfadeThinkingTransition(previous_state, state);
  if (transition_active) {
    transition_from_state = previous_state;
    transition_from_entered_ms = previous_entered_ms;
    transition_started_ms = now_ms;
  }
  displayed_state = state;
  state_entered_ms = now_ms;
  // Force the first frame of a new state on this loop iteration.
  last_frame_ms = now_ms - currentFrameIntervalMs();
}

void finishTransitionIfDue(const uint32_t now_ms) {
  if (!transition_active ||
      now_ms - transition_started_ms < kThinkingFadeOutMs) {
    return;
  }
  transition_active = false;
  state_entered_ms = now_ms;
  last_frame_ms = now_ms - currentFrameIntervalMs();
}

void applyPendingUpdates(const uint32_t now_ms) {
  if (pending.has_invalid_state) {
    pending.has_invalid_state = false;
    requested_state = StateId::ERROR;
    have_requested_state = true;
    // A correctly-versioned but invalid state is visible as ERROR. This is a
    // liveness signal too; a subsequent valid refresh recovers automatically.
    noteHeartbeat(now_ms);
  }

  if (pending.has_state) {
    requested_state = pending.state;
    active_count = pending.active_count;
    frame_interval_ms = pending.frame_interval_ms;
    offline_timeout_ms = pending.offline_timeout_ms;
    show_active_count = pending.show_active_count;
    have_requested_state = true;
    pending.has_state = false;
    // A full state refresh is also proof that the Linux daemon is alive.
    noteHeartbeat(now_ms);
  }

  if (pending.has_brightness) {
    brightness = pending.brightness;
    pending.has_brightness = false;
    last_frame_ms = now_ms - currentFrameIntervalMs();
  }

  if (pending.has_heartbeat) {
    pending.has_heartbeat = false;
    noteHeartbeat(now_ms);
  }
}

void updateDisplayedState(const uint32_t now_ms) {
  const bool online =
      have_heartbeat && (now_ms - last_heartbeat_ms <= offline_timeout_ms);
  if (!online) {
    enterState(OFFLINE, now_ms);
    return;
  }
  enterState(have_requested_state ? requested_state : IDLE, now_ms);
}

int32_t setStateRpc(const int32_t protocol_version, const int32_t state,
                    const int32_t sessions,
                    const int32_t requested_frame_interval_ms,
                    const int32_t requested_offline_timeout_ms,
                    const int32_t requested_show_count) {
  if (protocol_version != kProtocolVersion) {
    return RPC_REJECTED;
  }
  if (!isValidState(state)) {
    pending.has_state = false;
    pending.has_invalid_state = true;
    return RPC_REJECTED;
  }
  if (sessions < 0 || sessions > 255 ||
      requested_frame_interval_ms < kMinFrameIntervalMs ||
      requested_frame_interval_ms > kMaxFrameIntervalMs ||
      requested_offline_timeout_ms <
          static_cast<int32_t>(kMinOfflineTimeoutMs) ||
      requested_offline_timeout_ms >
          static_cast<int32_t>(kMaxOfflineTimeoutMs) ||
      (requested_show_count != 0 && requested_show_count != 1)) {
    return RPC_REJECTED;
  }

  // provide_safe runs this callback in the Arduino loop context. Keep it to a
  // bounded POD update; matrix drawing happens only in loop().
  pending.state = static_cast<StateId>(state);
  pending.active_count = static_cast<uint8_t>(sessions);
  pending.frame_interval_ms =
      static_cast<uint16_t>(requested_frame_interval_ms);
  pending.offline_timeout_ms =
      static_cast<uint32_t>(requested_offline_timeout_ms);
  pending.show_active_count = requested_show_count != 0;
  pending.has_invalid_state = false;
  pending.has_state = true;
  return RPC_OK;
}

int32_t heartbeatRpc(const int32_t protocol_version) {
  if (protocol_version != kProtocolVersion) {
    return RPC_REJECTED;
  }
  pending.has_heartbeat = true;
  return RPC_OK;
}

int32_t setBrightnessRpc(const int32_t protocol_version, const int32_t level) {
  if (protocol_version != kProtocolVersion) {
    return brightness;
  }
  const uint8_t effective =
      level < 0 ? 0
                : (level > kMaxBrightness ? kMaxBrightness
                                          : static_cast<uint8_t>(level));
  pending.brightness = effective;
  pending.has_brightness = true;
  return effective;
}

uint32_t getStatusRpc() {
  return packStatus(displayed_state, active_count, brightness);
}

uint32_t getVersionRpc() {
  return packVersion();
}

uint32_t getRenderMetricsRpc() {
  const uint16_t average_us =
      render_sample_count == 0
          ? 0
          : static_cast<uint16_t>(render_total_us / render_sample_count);
  return (static_cast<uint32_t>(average_us) << 16) | render_max_us;
}

void recordRenderDuration(const uint32_t duration_us) {
  const uint16_t bounded =
      duration_us > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(duration_us);
  if (render_sample_count >= 1024u) {
    render_total_us = 0;
    render_sample_count = 0;
    render_max_us = 0;
  }
  render_total_us += bounded;
  ++render_sample_count;
  if (bounded > render_max_us) {
    render_max_us = bounded;
  }
}

}  // namespace

void setup() {
  matrix_ready = matrix.begin() != 0;
  matrix.setGrayscaleBits(3);

  const uint32_t now_ms = millis();
  state_entered_ms = now_ms;
  last_frame_ms = now_ms - frame_interval_ms;
  renderAnimation(OFFLINE, now_ms, state_entered_ms, brightness, 0, false,
                  frame);
  if (matrix_ready) {
    matrix.draw(frame);
  }

  bridge_ready = Bridge.begin();
  if (bridge_ready) {
    bool all_registered = true;
    all_registered &=
        Bridge.provide_safe("codex_matrix_set_state", setStateRpc);
    all_registered &=
        Bridge.provide_safe("codex_matrix_heartbeat", heartbeatRpc);
    all_registered &=
        Bridge.provide_safe("codex_matrix_get_status", getStatusRpc);
    all_registered &=
        Bridge.provide_safe("codex_matrix_set_brightness", setBrightnessRpc);
    all_registered &=
        Bridge.provide_safe("codex_matrix_get_version", getVersionRpc);
    all_registered &= Bridge.provide_safe("codex_matrix_get_render_metrics",
                                          getRenderMetricsRpc);
    bridge_ready = all_registered;
  }
}

void loop() {
  const uint32_t now_ms = millis();
  applyPendingUpdates(now_ms);
  updateDisplayedState(now_ms);
  finishTransitionIfDue(now_ms);

  // Unsigned subtraction is intentionally used throughout so millis() wrap is
  // handled correctly.
  const uint16_t effective_frame_interval_ms = currentFrameIntervalMs();
  if (now_ms - last_frame_ms >= effective_frame_interval_ms) {
    last_frame_ms = now_ms;
    const uint32_t render_started_us = micros();
    if (transition_active) {
      renderTransition(transition_from_state, displayed_state, now_ms,
                       transition_from_entered_ms, transition_started_ms,
                       brightness, active_count, show_active_count, frame);
    } else {
      renderAnimation(displayed_state, now_ms, state_entered_ms, brightness,
                      active_count, show_active_count, frame);
    }
    if (matrix_ready) {
      matrix.draw(frame);
    }
    recordRenderDuration(micros() - render_started_us);
  }
}
