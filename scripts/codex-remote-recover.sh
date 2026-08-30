#!/bin/sh
set -eu

HOME_DIR=${HOME:-/home/arduino}
CODEX_BIN=${CODEX_BIN:-$HOME_DIR/.codex/packages/standalone/current/codex}
STATE_DIR=$HOME_DIR/.codex/app-server-daemon
CONTROL_SOCKET=$HOME_DIR/.codex/app-server-control/app-server-control.sock
SETTINGS_FILE=$STATE_DIR/settings.json

if [ ! -x "$CODEX_BIN" ]; then
  echo "Codex standalone binary not found: $CODEX_BIN" >&2
  echo "Install it with: curl -fsSL https://chatgpt.com/codex/install.sh | sh" >&2
  exit 1
fi

pid_from_file() {
  pid_file=$1
  [ -r "$pid_file" ] || return 1
  pid_record=$(cat "$pid_file" 2>/dev/null || true)
  case "$pid_record" in
    ''|*[!0-9]*)
      pid=$(printf '%s\n' "$pid_record" |
        sed -n 's/.*"pid"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p')
      ;;
    *) pid=$pid_record ;;
  esac
  case "$pid" in
    ''|*[!0-9]*) return 1 ;;
  esac
  printf '%s\n' "$pid"
}

pid_matches() {
  pid_file=$1
  required=$2
  pid=$(pid_from_file "$pid_file") || return 1
  kill -0 "$pid" 2>/dev/null || return 1
  [ -r "/proc/$pid/cmdline" ] || return 1
  command_line=$(tr '\000' ' ' < "/proc/$pid/cmdline")
  case "$command_line" in
    *codex*app-server*"$required"*) return 0 ;;
    *) return 1 ;;
  esac
}

remote_enabled() {
  [ -r "$SETTINGS_FILE" ] &&
    grep -Eq '"remoteControlEnabled"[[:space:]]*:[[:space:]]*true' "$SETTINGS_FILE"
}

healthy() {
  pid_matches "$app_pid_file" '--remote-control' || return 1
  pid_matches "$updater_pid_file" 'pid-update-loop' || return 1
  remote_enabled || return 1
  [ -S "$CONTROL_SOCKET" ] || return 1
  version_json=$($CODEX_BIN app-server daemon version 2>/dev/null) || return 1
  printf '%s\n' "$version_json" | grep -Eq '"status"[[:space:]]*:[[:space:]]*"running"'
}

app_pid_file=$STATE_DIR/app-server.pid
updater_pid_file=$STATE_DIR/app-server-updater.pid

# A live local daemon and updater are sufficient. Remote relay connectivity can
# legitimately be absent while the network is down and must not cause churn.
if healthy; then
  exit 0
fi

recover() {
  if pid_matches "$updater_pid_file" 'pid-update-loop'; then
    echo "Codex Remote health check failed; restarting the managed app-server."
    "$CODEX_BIN" app-server daemon restart
  else
    echo "Codex Remote updater is absent; bootstrapping durable remote control."
    "$CODEX_BIN" app-server daemon bootstrap --remote-control
  fi

  attempts=0
  while [ "$attempts" -lt 15 ]; do
    healthy && return 0
    attempts=$((attempts + 1))
    sleep 1
  done
  return 1
}

if recover; then
  exit 0
fi

# This fallback only removes dead PID records and an unowned stale socket. It
# deliberately avoids broad pkill patterns because the matrix quota reader and
# ASH READ may be using separate Codex processes.
echo "Codex Remote lifecycle recovery failed; cleaning verified stale state."
"$CODEX_BIN" app-server daemon stop >/dev/null 2>&1 || true
for pid_file in "$app_pid_file" "$updater_pid_file"; do
  if [ -e "$pid_file" ] && ! pid=$(pid_from_file "$pid_file"); then
    rm -f -- "$pid_file"
  elif [ -n "${pid:-}" ] && ! kill -0 "$pid" 2>/dev/null; then
    rm -f -- "$pid_file"
  fi
  pid=
done

if [ -e "$CONTROL_SOCKET" ] &&
   ! pgrep -u "$(id -u)" -f 'codex.*app-server.*--remote-control' >/dev/null 2>&1; then
  rm -f -- "$CONTROL_SOCKET"
fi

"$CODEX_BIN" app-server daemon bootstrap --remote-control

attempts=0
while [ "$attempts" -lt 15 ]; do
  healthy && exit 0
  attempts=$((attempts + 1))
  sleep 1
done

echo "Codex Remote is still unhealthy after bootstrap." >&2
exit 1
