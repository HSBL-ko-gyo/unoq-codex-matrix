#ifndef UNOQ_CODEX_MATRIX_ANIMATIONS_H
#define UNOQ_CODEX_MATRIX_ANIMATIONS_H

#include <stdint.h>

#include "protocol.h"

namespace unoq_codex_matrix {

// Renders exactly one 8x13 grayscale frame. The caller owns the static frame
// buffer; this function performs no dynamic allocation.
void renderAnimation(StateId state, uint32_t now_ms, uint32_t state_entered_ms,
                     uint8_t brightness, uint8_t active_count,
                     bool show_active_count, uint8_t frame[kPixelCount]);

}  // namespace unoq_codex_matrix

#endif  // UNOQ_CODEX_MATRIX_ANIMATIONS_H
