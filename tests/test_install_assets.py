from __future__ import annotations

import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def test_example_config_has_safe_documented_defaults() -> None:
    config = json.loads((ROOT / "config" / "config.example.json").read_text(encoding="utf-8"))
    assert config == {
        "brightness": 3,
        "frame_interval_ms": 100,
        "heartbeat_interval_s": 3,
        "offline_timeout_s": 12,
        "success_hold_s": 8,
        "transient_error_s": 1.5,
        "stale_session_s": 43200,
        "show_active_count": True,
        "log_level": "INFO",
    }


def test_systemd_unit_keeps_required_user_and_hardening() -> None:
    unit = (ROOT / "systemd" / "unoq-codex-matrix.service").read_text(encoding="utf-8")
    required = {
        "Description=UNO Q Codex LED Matrix Status Daemon",
        "Wants=arduino-router.service",
        "After=arduino-router.service",
        "Type=simple",
        "User=arduino",
        "Group=arduino",
        "RuntimeDirectory=unoq-codex-matrix",
        "RuntimeDirectoryMode=0750",
        "Restart=on-failure",
        "RestartSec=2",
        "NoNewPrivileges=true",
        "PrivateTmp=true",
        "WantedBy=multi-user.target",
    }
    assert required <= set(unit.splitlines())
    assert "/dev/ttyHS1" not in unit


def test_hook_example_covers_each_lifecycle_event_once() -> None:
    data = json.loads((ROOT / "hooks" / "hooks.example.json").read_text(encoding="utf-8"))
    hooks = data["hooks"]
    assert set(hooks) == {
        "SessionStart",
        "UserPromptSubmit",
        "PreToolUse",
        "PermissionRequest",
        "PostToolUse",
        "PreCompact",
        "PostCompact",
        "SubagentStart",
        "SubagentStop",
        "Stop",
        "SessionEnd",
    }
    for groups in hooks.values():
        assert len(groups) == 1
        assert groups[0]["hooks"] == [
            {
                "type": "command",
                "command": "/usr/local/bin/unoq-codex-matrix-hook",
                "timeout": 1,
            }
        ]


def test_install_scripts_do_not_contain_forbidden_system_mutations() -> None:
    scripts = "\n".join(
        path.read_text(encoding="utf-8")
        for path in sorted((ROOT / "scripts").iterdir())
        if path.is_file()
    )
    assert re.search(r"\b(?:apt|apt-get)\s+(?:full-upgrade|dist-upgrade|upgrade)\b", scripts) is None
    assert re.search(r"\bchmod\s+777\b", scripts) is None
    assert "/dev/ttyHS1" not in scripts
    assert "auth.json" not in scripts
