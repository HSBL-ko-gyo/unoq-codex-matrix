#!/bin/sh
set -u

RECOVER_BIN=${RECOVER_BIN:-/usr/local/libexec/unoq-codex-remote-recover}
CODEX_BIN=${CODEX_BIN:-/home/arduino/.codex/packages/standalone/current/codex}
CHECK_INTERVAL_SEC=${CHECK_INTERVAL_SEC:-60}
RECOVERY_TIMEOUT_SEC=${RECOVERY_TIMEOUT_SEC:-150}
TIME_SYNC_WAIT_SEC=${TIME_SYNC_WAIT_SEC:-120}

case "$CHECK_INTERVAL_SEC" in
  ''|*[!0-9]*) echo "Invalid CHECK_INTERVAL_SEC" >&2; exit 2 ;;
esac

case "$RECOVERY_TIMEOUT_SEC" in
  ''|*[!0-9]*) echo "Invalid RECOVERY_TIMEOUT_SEC" >&2; exit 2 ;;
esac

case "$TIME_SYNC_WAIT_SEC" in
  ''|*[!0-9]*) echo "Invalid TIME_SYNC_WAIT_SEC" >&2; exit 2 ;;
esac

stop_monitor() {
  echo "Stopping Codex Remote monitor."
  "$CODEX_BIN" app-server daemon stop >/dev/null 2>&1 || true
  exit 0
}

trap stop_monitor TERM INT HUP

echo "Codex Remote monitor started (interval=${CHECK_INTERVAL_SEC}s)."

# The 0.147 daemon records a wall-clock process start time in its PID file.
# UNO Q can correct its clock after network-online.target, invalidating an
# otherwise live process record. Wait for the first NTP synchronization before
# bootstrapping, with a bounded timeout so a blocked NTP server cannot hang boot.
if command -v timedatectl >/dev/null 2>&1 &&
   [ "$(timedatectl show -p NTPSynchronized --value 2>/dev/null || true)" != yes ]; then
  echo "Waiting up to ${TIME_SYNC_WAIT_SEC}s for initial clock synchronization."
  waited=0
  while [ "$waited" -lt "$TIME_SYNC_WAIT_SEC" ]; do
    [ "$(timedatectl show -p NTPSynchronized --value 2>/dev/null || true)" = yes ] && break
    sleep 5
    waited=$((waited + 5))
  done
  if [ "$(timedatectl show -p NTPSynchronized --value 2>/dev/null || true)" = yes ]; then
    echo "Initial clock synchronization completed."
  else
    echo "Clock synchronization wait timed out; recovery will continue at the normal interval." >&2
  fi
fi

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
