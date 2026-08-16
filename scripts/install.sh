#!/bin/sh
set -eu

NO_FLASH=0
NO_HOOKS=0
NO_START=0
for option in "$@"; do
  case "$option" in
    --no-flash) NO_FLASH=1 ;;
    --no-hooks) NO_HOOKS=1 ;;
    --no-start) NO_START=1 ;;
    *) echo "Unknown option: $option" >&2; exit 2 ;;
  esac
done

if [ "$(id -u)" -ne 0 ]; then
  echo "Run this installer as root (for example: sudo scripts/install.sh)." >&2
  exit 1
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
INSTALL_ROOT=/opt/unoq-codex-matrix
OWNER_MARKER="$INSTALL_ROOT/.unoq-codex-matrix-owner"
OWNER_TOKEN=unoq-codex-matrix-install-root-v1
VENV="$INSTALL_ROOT/venv"
HOOK_INSTALLED="$INSTALL_ROOT/bin/unoq-codex-matrix-hook"
HOOK_TARGET=/usr/local/bin/unoq-codex-matrix-hook
UNIT_TARGET=/etc/systemd/system/unoq-codex-matrix.service
CONFIG_DIR=/etc/unoq-codex-matrix
CONFIG_TARGET="$CONFIG_DIR/config.json"
CONFIG_CREATED_MARKER="$INSTALL_ROOT/.config-created-by-unoq-codex-matrix"
ARDUINO_HOME=/home/arduino

command -v python3 >/dev/null
command -v arduino-cli >/dev/null
command -v systemctl >/dev/null
id arduino >/dev/null 2>&1

# Refuse all ambiguous ownership before making persistent changes.
if [ -e "$INSTALL_ROOT" ] || [ -L "$INSTALL_ROOT" ]; then
  if [ ! -d "$INSTALL_ROOT" ] || [ -L "$INSTALL_ROOT" ] ||
     [ ! -f "$OWNER_MARKER" ] || [ -L "$OWNER_MARKER" ] ||
     ! grep -Fqx "$OWNER_TOKEN" "$OWNER_MARKER"; then
    echo "Refusing to use unowned install root: $INSTALL_ROOT" >&2
    exit 1
  fi
fi
if [ -L "$VENV" ] || { [ -e "$VENV" ] && [ ! -d "$VENV" ]; }; then
  echo "Refusing unexpected venv path: $VENV" >&2
  exit 1
fi
if [ -L "$INSTALL_ROOT/bin" ] ||
   { [ -e "$INSTALL_ROOT/bin" ] && [ ! -d "$INSTALL_ROOT/bin" ]; }; then
  echo "Refusing unexpected bin path: $INSTALL_ROOT/bin" >&2
  exit 1
fi
for executable in unoq-codex-matrix unoq-codex-matrixd; do
  target="/usr/local/bin/$executable"
  expected="$VENV/bin/$executable"
  if [ -L "$target" ]; then
    [ "$(readlink "$target")" = "$expected" ] || {
      echo "Refusing to replace unrelated symlink: $target" >&2
      exit 1
    }
  elif [ -e "$target" ]; then
    echo "Refusing to replace existing file: $target" >&2
    exit 1
  fi
done
if [ -L "$HOOK_TARGET" ]; then
  hook_link=$(readlink "$HOOK_TARGET")
  case "$hook_link" in
    "$HOOK_INSTALLED"|"$VENV/bin/unoq-codex-matrix-hook") ;;
    *) echo "Refusing to replace unrelated symlink: $HOOK_TARGET" >&2; exit 1 ;;
  esac
elif [ -e "$HOOK_TARGET" ] && ! grep -q '^# UNOQ_CODEX_MATRIX_HOOK=1$' "$HOOK_TARGET"; then
  echo "Refusing to replace existing file: $HOOK_TARGET" >&2
  exit 1
fi
if [ -e "$UNIT_TARGET" ] || [ -L "$UNIT_TARGET" ]; then
  if [ -L "$UNIT_TARGET" ] ||
     ! grep -q '^# UNOQ_CODEX_MATRIX_SERVICE=1$' "$UNIT_TARGET"; then
    echo "Refusing to replace unowned systemd unit: $UNIT_TARGET" >&2
    exit 1
  fi
fi
if [ -e "$CONFIG_DIR" ] || [ -L "$CONFIG_DIR" ]; then
  if [ ! -d "$CONFIG_DIR" ] || [ -L "$CONFIG_DIR" ]; then
    echo "Refusing unexpected config directory: $CONFIG_DIR" >&2
    exit 1
  fi
fi
if [ -L "$CONFIG_TARGET" ]; then
  echo "Refusing symlinked config file: $CONFIG_TARGET" >&2
  exit 1
fi

APT_INDEX_UPDATED=0
install_if_missing() {
  command_name=$1
  package_name=$2
  if ! command -v "$command_name" >/dev/null 2>&1; then
    if [ "$APT_INDEX_UPDATED" -eq 0 ]; then
      apt-get update
      APT_INDEX_UPDATED=1
    fi
    apt-get install --no-install-recommends -y "$package_name"
  fi
}

# `venv --help` succeeds on Debian even when ensurepip is absent.  Prove that
# an isolated environment with pip can actually be created, then install only
# python3-venv if that capability is missing.
venv_probe=$(mktemp -d "${TMPDIR:-/tmp}/unoq-venv-probe.XXXXXXXX")
cleanup_venv_probe() {
  case "$venv_probe" in
    "${TMPDIR:-/tmp}"/unoq-venv-probe.*) rm -rf -- "$venv_probe" ;;
  esac
}
venv_capable() {
  cleanup_venv_probe
  mkdir -m 0700 "$venv_probe"
  python3 -m venv "$venv_probe/venv" >/dev/null 2>&1 &&
    "$venv_probe/venv/bin/python" -m pip --version >/dev/null 2>&1
}
if ! venv_capable; then
  cleanup_venv_probe
  install_if_missing __unoq_python3_venv_missing__ python3-venv
  venv_capable || {
    cleanup_venv_probe
    echo "python3-venv was installed but a pip-enabled venv still cannot be created." >&2
    exit 1
  }
fi
cleanup_venv_probe

mkdir -p "$INSTALL_ROOT"
if [ ! -e "$OWNER_MARKER" ]; then
  printf '%s\n' "$OWNER_TOKEN" >"$OWNER_MARKER"
  chmod 0644 "$OWNER_MARKER"
fi
if [ ! -x "$VENV/bin/python" ]; then
  python3 -m venv "$VENV"
fi
"$VENV/bin/python" -m pip install "$REPO_ROOT"

for executable in unoq-codex-matrix unoq-codex-matrixd; do
  ln -sfn "$VENV/bin/$executable" "/usr/local/bin/$executable"
done

native_candidate=$(mktemp "${TMPDIR:-/tmp}/unoq-codex-matrix-hook.XXXXXXXX")
cleanup_native_candidate() {
  case "$native_candidate" in
    "${TMPDIR:-/tmp}"/unoq-codex-matrix-hook.*) rm -f -- "$native_candidate" ;;
  esac
}
trap cleanup_native_candidate EXIT HUP INT TERM
if "$REPO_ROOT/scripts/build-native-hook.sh" "$native_candidate" >/dev/null; then
  hook_source=$native_candidate
  echo "Using the native low-latency lifecycle hook."
else
  echo "Native hook build failed; installing the portable shell fallback." >&2
  install_if_missing jq jq
  install_if_missing socat socat
  hook_source="$REPO_ROOT/scripts/unoq-codex-matrix-hook"
fi
mkdir -p "$INSTALL_ROOT/bin"
install -m 0755 "$hook_source" "$HOOK_INSTALLED"
if [ -e "$HOOK_TARGET" ] && [ ! -L "$HOOK_TARGET" ]; then
  rm -f -- "$HOOK_TARGET"
fi
ln -sfn "$HOOK_INSTALLED" "$HOOK_TARGET"

mkdir -p "$CONFIG_DIR"
if [ ! -e "$CONFIG_TARGET" ]; then
  install -m 0644 "$REPO_ROOT/config/config.example.json" "$CONFIG_TARGET"
  : >"$CONFIG_CREATED_MARKER"
fi
install -m 0644 "$REPO_ROOT/systemd/unoq-codex-matrix.service" "$UNIT_TARGET"
systemctl daemon-reload

if [ "$NO_HOOKS" -eq 0 ]; then
  runuser -u arduino -- "$VENV/bin/python" "$REPO_ROOT/scripts/install-hooks.py" --home "$ARDUINO_HOME"
fi
if [ "$NO_START" -eq 0 ]; then
  systemctl enable --now unoq-codex-matrix.service
fi
if [ "$NO_FLASH" -eq 0 ]; then
  runuser -u arduino -- "$REPO_ROOT/scripts/flash-firmware.sh"
fi
if [ "$NO_START" -eq 0 ]; then
  systemctl restart unoq-codex-matrix.service
  sleep 2
  "$VENV/bin/unoq-codex-matrix" demo
  "$VENV/bin/unoq-codex-matrix" doctor
fi

echo "Installation complete. Review hook trust in Codex with /hooks."
