#ifndef UNOQ_CODEX_MATRIX_FRAMES_H
#define UNOQ_CODEX_MATRIX_FRAMES_H

#include <stdint.h>

#include "protocol.h"

namespace unoq_codex_matrix {
namespace frames {

// A set bit at position x lights column x. These independent, hand-authored
// glyphs are deliberately small so animation code can select the intensity.
// Permission waiting is a centered question mark. Rows 0 and 7 stay empty
// because they are reserved for the quota bar and active-session indicator.
static constexpr uint16_t kQuestionMark[kMatrixHeight] = {
    0,
    (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8),
    (1u << 3) | (1u << 9),
    (1u << 8) | (1u << 9),
    (1u << 6) | (1u << 7) | (1u << 8),
    0,
    (1u << 6) | (1u << 7),
    0,
};

static constexpr uint16_t kCheckMark[kMatrixHeight] = {
    0,
    (1u << 10),
    (1u << 9) | (1u << 10),
    (1u << 2) | (1u << 8) | (1u << 9),
    (1u << 3) | (1u << 7) | (1u << 8),
    (1u << 4) | (1u << 6) | (1u << 7),
    (1u << 5) | (1u << 6),
    0,
};

static constexpr uint16_t kErrorX[kMatrixHeight] = {
    (1u << 2) | (1u << 10),
    (1u << 3) | (1u << 9),
    (1u << 4) | (1u << 8),
    (1u << 5) | (1u << 7),
    (1u << 6),
    (1u << 5) | (1u << 7),
    (1u << 4) | (1u << 8),
    (1u << 3) | (1u << 9),
};

// Exclamation mark above two separated cable ends.
static constexpr uint16_t kOffline[kMatrixHeight] = {
    (1u << 6),
    (1u << 6),
    (1u << 6),
    (1u << 6),
    0,
    (1u << 6),
    (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) |
        (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11),
    (1u << 0) | (1u << 5) | (1u << 7) | (1u << 12),
};

}  // namespace frames
}  // namespace unoq_codex_matrix

#endif  // UNOQ_CODEX_MATRIX_FRAMES_H
