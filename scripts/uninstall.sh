#!/bin/sh
set -eu

PURGE=0
case "${1:-}" in
  "") ;;
  --purge) PURGE=1 ;;
  *) echo "Usage: $0 [--purge]" >&2; exit 2 ;;
esac

if [ "$(id -u)" -ne 0 ]; then
  echo "Run this uninstaller as root (for example: sudo scripts/uninstall.sh)." >&2
  exit 1
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
INSTALL_ROOT=/opt/unoq-codex-matrix
OWNER_MARKER="$INSTALL_ROOT/.unoq-codex-matrix-owner"
OWNER_TOKEN=unoq-codex-matrix-install-root-v1
UNIT_TARGET=/etc/systemd/system/unoq-codex-matrix.service
CONFIG_TARGET=/etc/unoq-codex-matrix/config.json
CONFIG_CREATED_MARKER="$INSTALL_ROOT/.config-created-by-unoq-codex-matrix"

root_owned=0
if [ -d "$INSTALL_ROOT" ] && [ ! -L "$INSTALL_ROOT" ] &&
   [ -f "$OWNER_MARKER" ] && [ ! -L "$OWNER_MARKER" ] &&
   grep -Fqx "$OWNER_TOKEN" "$OWNER_MARKER"; then
  root_owned=1
fi
config_created=0
if [ "$root_owned" -eq 1 ] && [ -f "$CONFIG_CREATED_MARKER" ] &&
   [ ! -L "$CONFIG_CREATED_MARKER" ]; then
  config_created=1
fi

if id arduino >/dev/null 2>&1; then
  runuser -u arduino -- python3 "$REPO_ROOT/scripts/uninstall-hooks.py" --home /home/arduino || true
fi

unit_removed=0
if [ -f "$UNIT_TARGET" ] && [ ! -L "$UNIT_TARGET" ] &&
   grep -q '^# UNOQ_CODEX_MATRIX_SERVICE=1$' "$UNIT_TARGET"; then
  systemctl disable --now unoq-codex-matrix.service 2>/dev/null || true
  rm -f -- "$UNIT_TARGET"
  unit_removed=1
elif [ -e "$UNIT_TARGET" ] || [ -L "$UNIT_TARGET" ]; then
  echo "Preserving unowned systemd unit: $UNIT_TARGET" >&2
fi
[ "$unit_removed" -eq 0 ] || systemctl daemon-reload

if [ "$root_owned" -eq 1 ]; then
  for executable in unoq-codex-matrix unoq-codex-matrixd; do
    target="/usr/local/bin/$executable"
    expected="$INSTALL_ROOT/venv/bin/$executable"
    if [ -L "$target" ] && [ "$(readlink "$target")" = "$expected" ]; then
      rm -f -- "$target"
    fi
  done
  hook_target=/usr/local/bin/unoq-codex-matrix-hook
  if [ -L "$hook_target" ] &&
     [ "$(readlink "$hook_target")" = "$INSTALL_ROOT/bin/unoq-codex-matrix-hook" ]; then
    rm -f -- "$hook_target"
  fi
fi

# Migrate/remove only the old, self-marked portable hook file.
hook_target=/usr/local/bin/unoq-codex-matrix-hook
if [ -f "$hook_target" ] && [ ! -L "$hook_target" ] &&
   grep -q '^# UNOQ_CODEX_MATRIX_HOOK=1$' "$hook_target"; then
  rm -f -- "$hook_target"
fi

if [ "$root_owned" -eq 1 ]; then
  rm -rf -- "$INSTALL_ROOT"
elif [ -e "$INSTALL_ROOT" ] || [ -L "$INSTALL_ROOT" ]; then
  echo "Preserving unowned install root: $INSTALL_ROOT" >&2
fi
if [ "$PURGE" -eq 1 ] && [ "$config_created" -eq 1 ]; then
  rm -f -- "$CONFIG_TARGET"
  rmdir /etc/unoq-codex-matrix 2>/dev/null || true
fi

echo "Removed this project's owned daemon files and Hook entries. MCU firmware and packages were left unchanged."
