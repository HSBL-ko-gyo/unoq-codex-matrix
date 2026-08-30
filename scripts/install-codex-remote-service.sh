#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
  echo "Run as root: sudo sh scripts/install-codex-remote-service.sh" >&2
  exit 1
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
RECOVER_TARGET=/usr/local/libexec/unoq-codex-remote-recover
UNIT_TARGET=/etc/systemd/system/unoq-codex-remote.service

command -v systemctl >/dev/null
id arduino >/dev/null 2>&1

mkdir -p /usr/local/libexec
install -m 0755 "$REPO_ROOT/scripts/codex-remote-recover.sh" "$RECOVER_TARGET"
install -m 0644 "$REPO_ROOT/systemd/unoq-codex-remote.service" "$UNIT_TARGET"
systemctl daemon-reload
systemctl enable unoq-codex-remote.service

# Migrate any currently running detached Codex daemon under systemd ownership.
# Failure is non-fatal because the boot service can still recover it later.
CODEX_BIN=/home/arduino/.codex/packages/standalone/current/codex
if [ -x "$CODEX_BIN" ]; then
  runuser -u arduino -- env HOME=/home/arduino "$CODEX_BIN" app-server daemon stop >/dev/null 2>&1 || true
fi

systemctl restart unoq-codex-remote.service

echo "Codex Remote Control boot recovery is enabled."
systemctl --no-pager --full status unoq-codex-remote.service || true
