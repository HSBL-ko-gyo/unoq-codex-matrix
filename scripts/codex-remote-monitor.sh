#!/bin/sh
set -u

RECOVER_BIN=${RECOVER_BIN:-/usr/local/libexec/unoq-codex-remote-recover}
CODEX_BIN=${CODEX_BIN:-/home/arduino/.codex/packages/standalone/current/codex}
CHECK_INTERVAL_SEC=${CHECK_INTERVAL_SEC:-60}
RECOVERY_TIMEOUT_SEC=${RECOVERY_TIMEOUT_SEC:-150}

case "$CHECK_INTERVAL_SEC" in
  ''|*[!0-9]*) echo "Invalid CHECK_INTERVAL_SEC" >&2; exit 2 ;;
esac

case "$RECOVERY_TIMEOUT_SEC" in
  ''|*[!0-9]*) echo "Invalid RECOVERY_TIMEOUT_SEC" >&2; exit 2 ;;
esac

stop_monitor() {
  echo "Stopping Codex Remote monitor."
  "$CODEX_BIN" app-server daemon stop >/dev/null 2>&1 || true
  exit 0
}

trap stop_monitor TERM INT HUP

echo "Codex Remote monitor started (interval=${CHECK_INTERVAL_SEC}s)."
while :; do
  if command -v timeout >/dev/null 2>&1; then
    timeout --signal=TERM --kill-after=10s "$RECOVERY_TIMEOUT_SEC" "$RECOVER_BIN"
    result=$?
  else
    "$RECOVER_BIN"
    result=$?
  fi

  if [ "$result" -ne 0 ]; then
    echo "Codex Remote recovery attempt failed (status=$result); retrying after ${CHECK_INTERVAL_SEC}s." >&2
  fi

  sleep "$CHECK_INTERVAL_SEC" &
  wait "$!" || stop_monitor
done
