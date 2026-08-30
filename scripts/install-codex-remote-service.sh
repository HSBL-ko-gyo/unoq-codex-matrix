#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
  echo "Run as root: sudo sh scripts/install-codex-remote-service.sh" >&2
  exit 1
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
RECOVER_TARGET=/usr/local/libexec/unoq-codex-remote-recover
MONITOR_TARGET=/usr/local/libexec/unoq-codex-remote-monitor
UNIT_TARGET=/etc/systemd/system/unoq-codex-remote.service

command -v systemctl >/dev/null
id arduino >/dev/null 2>&1

mkdir -p /usr/local/libexec
install -m 0755 "$REPO_ROOT/scripts/codex-remote-recover.sh" "$RECOVER_TARGET"
install -m 0755 "$REPO_ROOT/scripts/codex-remote-monitor.sh" "$MONITOR_TARGET"
install -m 0644 "$REPO_ROOT/systemd/unoq-codex-remote.service" "$UNIT_TARGET"

# Disable only the known legacy user service that directly launches the same
# remote-control app-server. Keep its file for rollback and leave all other user
# services, including ASH READ and the persistent Codex CLI, untouched.
user_systemctl() {
  runuser -u arduino -- env \
    HOME=/home/arduino \
    XDG_RUNTIME_DIR=/run/user/1000 \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
    systemctl --user "$@"
}

legacy_exec=$(user_systemctl show codex-remote-control.service -p ExecStart --value 2>/dev/null || true)
case "$legacy_exec" in
  *codex*app-server*--remote-control*)
    user_systemctl disable --now codex-remote-control.service || true
    echo "Disabled legacy codex-remote-control.service; its unit file was preserved."
    ;;
esac

# Migrate any official detached daemon under the monitor service's cgroup.
CODEX_BIN=/home/arduino/.codex/packages/standalone/current/codex
if [ -x "$CODEX_BIN" ]; then
  runuser -u arduino -- env HOME=/home/arduino "$CODEX_BIN" app-server daemon stop >/dev/null 2>&1 || true
fi

systemctl daemon-reload
systemctl enable unoq-codex-remote.service
systemctl restart unoq-codex-remote.service

echo "Codex Remote Control boot recovery and 60-second monitoring are enabled."
systemctl --no-pager --full status unoq-codex-remote.service || true
