from __future__ import annotations

from pathlib import Path
import re
import shutil
import subprocess

import pytest


ROOT = Path(__file__).parents[1]
INSTALL = (ROOT / "scripts" / "install.sh").read_text(encoding="utf-8")
UNINSTALL = (ROOT / "scripts" / "uninstall.sh").read_text(encoding="utf-8")
UNIT = (ROOT / "systemd" / "unoq-codex-matrix.service").read_text(encoding="utf-8")


def _owner_token(script: str) -> str:
    match = re.search(r"^OWNER_TOKEN=([^\n]+)$", script, re.MULTILINE)
    assert match is not None
    return match.group(1)


def test_install_and_uninstall_share_an_exact_owner_marker() -> None:
    assert _owner_token(INSTALL) == _owner_token(UNINSTALL)
    assert ".unoq-codex-matrix-owner" in INSTALL
    assert ".unoq-codex-matrix-owner" in UNINSTALL
    assert 'grep -Fqx "$OWNER_TOKEN" "$OWNER_MARKER"' in INSTALL
    assert 'grep -Fqx "$OWNER_TOKEN" "$OWNER_MARKER"' in UNINSTALL


def test_installer_refuses_unowned_links_root_and_service() -> None:
    assert "Refusing to use unowned install root" in INSTALL
    assert "Refusing to replace unrelated symlink" in INSTALL
    assert "Refusing to replace unowned systemd unit" in INSTALL
    assert "$(readlink \"$target\")" in INSTALL
    assert "# UNOQ_CODEX_MATRIX_SERVICE=1" in UNIT


def test_uninstaller_preserves_every_unowned_destructive_target() -> None:
    assert "Preserving unowned install root" in UNINSTALL
    assert "Preserving unowned systemd unit" in UNINSTALL
    assert 'if [ "$root_owned" -eq 1 ]; then\n  rm -rf -- "$INSTALL_ROOT"' in UNINSTALL
    assert 'grep -q \'^# UNOQ_CODEX_MATRIX_SERVICE=1$\'' in UNINSTALL
    assert '"$(readlink "$target")" = "$expected"' in UNINSTALL


def test_installer_probes_a_real_pip_enabled_venv_and_limits_packages() -> None:
    assert 'python3 -m venv "$venv_probe/venv"' in INSTALL
    assert '"$venv_probe/venv/bin/python" -m pip --version' in INSTALL
    assert "python3 -m venv --help" not in INSTALL
    assert "apt-get install --no-install-recommends -y" in INSTALL
    lowered = (INSTALL + UNINSTALL).lower()
    assert "apt upgrade" not in lowered
    assert "full-upgrade" not in lowered
    assert "dist-upgrade" not in lowered
    assert "chmod 777" not in lowered


@pytest.mark.skipif(shutil.which("sh") is None, reason="POSIX shell is unavailable")
@pytest.mark.parametrize(
    "name",
    [
        "install.sh",
        "uninstall.sh",
        "build-native-hook.sh",
        "codex-remote-recover.sh",
        "codex-remote-monitor.sh",
        "install-codex-remote-service.sh",
    ],
)
def test_shell_scripts_parse(name: str) -> None:
    completed = subprocess.run(
        ["sh", "-n", str(ROOT / "scripts" / name)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=10,
    )
    assert completed.returncode == 0, completed.stderr.decode(errors="replace")
