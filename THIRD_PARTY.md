# Third-party notices

UNO Q Codex Matrix is an independent implementation distributed under the MIT
License. The repository does not contain source code, binaries, assets, USB
identifiers, or protocol implementations copied from the community projects
listed below.

The projects were reviewed to understand existing approaches. Their names,
licenses at the reviewed revisions, and the high-level concepts considered are
recorded for attribution and license hygiene.

| Project | Reviewed revision | License at that revision | Concept reviewed |
|---|---|---|---|
| [Pixelmoss/codex-kick75-status-lights](https://github.com/Pixelmoss/codex-kick75-status-lights/tree/e32648ee86a8a729734060ac09bd7f8a1213876f) | `e32648e` | [MIT](https://github.com/Pixelmoss/codex-kick75-status-lights/blob/e32648ee86a8a729734060ac09bd7f8a1213876f/LICENSE) | Short-lived Hook-to-daemon handoff, fail-open behavior, per-session state, priority, and expiry |
| [GFlash6/codex-status-LED](https://github.com/GFlash6/codex-status-LED/tree/850c63531f5ad769b4ea658219d8e14b0f3f9acd) | `850c635` | [MIT](https://github.com/GFlash6/codex-status-LED/blob/850c63531f5ad769b4ea658219d8e14b0f3f9acd/LICENSE) | Hook forwarding to a long-lived status process and completion transitions |
| [k33bs/qmk-codex-status](https://github.com/k33bs/qmk-codex-status/tree/84bc3472c47ae29b36bf1163ef44b5012a0063c3) | `84bc347` | [GPL-2.0-or-later](https://github.com/k33bs/qmk-codex-status/blob/84bc3472c47ae29b36bf1163ef44b5012a0063c3/README.md#license) | Desktop-to-keyboard Vendor HID status slots and the limits of color/effect status transport |
| [imliubo/codex-micro-4-core2](https://github.com/imliubo/codex-micro-4-core2/tree/2ee23a4ab696f94bb78d250f28cc4a9b879ba079) | `2ee23a4` | [MIT](https://github.com/imliubo/codex-micro-4-core2/blob/2ee23a4ab696f94bb78d250f28cc4a9b879ba079/LICENSE) | BLE Vendor HID status slots for a Codex Micro-like display |
| [hu619340515/rp2040-zero-onboard-led-codex-light](https://github.com/hu619340515/rp2040-zero-onboard-led-codex-light/tree/a32ed283b8a64615e088a913835bbe4663973624) | `a32ed28` | No license file or grant detected | Fail-open local status forwarding, permission and completion signaling, and source-priority behavior |
| [huxun1978/codex_led_state](https://github.com/huxun1978/codex_led_state/tree/52b6242e41913efa055070afb96f2318ea6687a6) | `52b6242` | No license file or grant detected | Browser-derived coarse THINK/DONE/IDLE signaling to an LED endpoint |

“No license detected” is not a permissive license. No code or creative assets
from those repositories were used. In particular, GPL-licensed code from
`qmk-codex-status` was not incorporated into this MIT project.

The detailed design comparison, including ideas deliberately not adopted, is in
[docs/prior-art.md](docs/prior-art.md).

## Runtime and build dependencies

This project interoperates with software installed separately on the UNO Q,
including [Arduino Router](https://github.com/arduino/arduino-router),
[Arduino_RouterBridge](https://github.com/arduino-libraries/Arduino_RouterBridge),
the Arduino UNO Q core and matrix library, Python, and MessagePack. It also uses
the Python packages declared in `pyproject.toml`. These components are not
relicensed by this repository; each remains subject to its upstream license.

Generated build artifacts and dependency source trees are not committed here.
