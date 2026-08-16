#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
SKETCH="$REPO_ROOT/firmware/unoq_codex_matrix"
BUILD_DIR=$(mktemp -d /tmp/unoq-codex-matrix-build.XXXXXX)
trap 'rm -rf -- "$BUILD_DIR"' EXIT HUP INT TERM

arduino-cli lib install Arduino_RouterBridge@0.4.3
arduino-cli compile -b arduino:zephyr:unoq --output-dir "$BUILD_DIR" "$SKETCH"
arduino-cli upload -b arduino:zephyr:unoq --input-dir "$BUILD_DIR" "$SKETCH"
