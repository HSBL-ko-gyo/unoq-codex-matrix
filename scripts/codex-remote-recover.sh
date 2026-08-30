#!/bin/sh
set -eu

HOME_DIR=${HOME:-/home/arduino}
CODEX_BIN=${CODEX_BIN:-$HOME_DIR/.codex/packages/standalone/current/codex}
STATE_DIR=$HOME_DIR/.codex/app-server-daemon

if [ ! -x "$CODEX_BIN" ]; then
  echo "Codex standalone binary not found: $CODEX_BIN" >&2
  echo "Install it with: curl -fsSL https://chatgpt.com/codex/install.sh | sh" >&2
  exit 1
fi

pid_alive() {
  pid_file=$1
  [ -r "$pid_file" ] || return 1
  pid=$(cat "$pid_file" 2>/dev/null || true)
  case "$pid" in
    ''|*[!0-9]*) return 1 ;;
  esac
  kill -0 "$pid" 2>/dev/null
}

app_pid_file=$STATE_DIR/app-server.pid
updater_pid_file=$STATE_DIR/app-server-updater.pid

# Healthy bootstrap state: both the app-server and updater are alive.
if pid_alive "$app_pid_file" && pid_alive "$updater_pid_file"; then
  exit 0
fi

recover() {
  if pid_alive "$updater_pid_file"; then
    "$CODEX_BIN" app-server daemon start
  else
    "$CODEX_BIN" app-server daemon bootstrap --remote-control
  fi
}

if recover; then
  exit 0
fi

# Known Codex failure modes can leave stale daemon state or an unmanaged
# app-server behind. This path is only reached when the normal lifecycle
# command failed, so clear the stale process/socket state and bootstrap again.
"$CODEX_BIN" app-server daemon stop >/dev/null 2>&1 || true
pkill -u "$(id -u)" -f "$CODEX_BIN app-server" >/dev/null 2>&1 || true
rm -f -- "$HOME_DIR/.codex/app-server-control/app-server-control.sock"
"$CODEX_BIN" app-server daemon bootstrap --remote-control
