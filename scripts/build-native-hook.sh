#!/bin/sh
# Build the short-lived native hook without installing compiler packages.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
SOURCE="$REPO_ROOT/native/unoq-codex-matrix-hook.c"
OUTPUT=${1:-"$REPO_ROOT/build/unoq-codex-matrix-hook"}
OUTPUT_DIR=$(dirname -- "$OUTPUT")
mkdir -p "$OUTPUT_DIR"

build_with_cc() {
  compiler=$1
  "$compiler" -std=c11 -O2 -Wall -Wextra -Werror \
    -fno-ident -fno-asynchronous-unwind-tables \
    "$SOURCE" -o "$OUTPUT"
}

if command -v cc >/dev/null 2>&1; then
  build_with_cc cc
elif command -v gcc >/dev/null 2>&1; then
  build_with_cc gcc
elif command -v clang >/dev/null 2>&1; then
  build_with_cc clang
else
  # Debian's TCC package is small and native.  Downloading and extracting it
  # plus libc development files into a private temporary directory avoids any
  # system package or configuration change on a compiler-free UNO Q image.
  command -v apt >/dev/null 2>&1
  command -v dpkg-deb >/dev/null 2>&1
  temporary=$(mktemp -d "${TMPDIR:-/tmp}/unoq-hook-build.XXXXXXXX")
  cleanup() {
    case "$temporary" in
      "${TMPDIR:-/tmp}"/unoq-hook-build.*) rm -rf -- "$temporary" ;;
    esac
  }
  trap cleanup EXIT HUP INT TERM
  (
    cd "$temporary"
    apt download tcc libc6-dev linux-libc-dev >/dev/null
    for package in ./*.deb; do
      dpkg-deb -x "$package" root
    done
  )
  compiler="$temporary/root/usr/bin/tcc"
  crt_path=$(find "$temporary/root/usr/lib" -type f -name crt1.o -print -quit)
  [ -n "$crt_path" ]
  system_lib=$(dirname -- "$crt_path")
  architecture=$(basename -- "$system_lib")
  tcc_lib="$system_lib/tcc"
  [ -x "$compiler" ]
  [ -f "$system_lib/crt1.o" ]
  "$compiler" -std=c11 -O2 -Wall -Werror \
    -I"$tcc_lib/include" \
    -I"$temporary/root/usr/include/$architecture" \
    -I"$temporary/root/usr/include" \
    -nostdlib "$system_lib/crt1.o" "$system_lib/crti.o" \
    "$SOURCE" "$tcc_lib/libtcc1.a" -L"$system_lib" -lc \
    "$system_lib/crtn.o" -o "$OUTPUT"
fi

chmod 0755 "$OUTPUT"
"$OUTPUT" </dev/null >/dev/null 2>&1
printf '%s\n' "$OUTPUT"
