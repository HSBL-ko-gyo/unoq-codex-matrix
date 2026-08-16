from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).parents[1]
INSTALL = ROOT / "scripts" / "install-hooks.py"
UNINSTALL = ROOT / "scripts" / "uninstall-hooks.py"
EVENTS = {
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


def _run(script: Path, home: Path, command: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(script), "--home", str(home), "--command", command],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=5,
        check=False,
    )


def _commands(group: object) -> list[str]:
    if not isinstance(group, dict) or not isinstance(group.get("hooks"), list):
        return []
    return [
        item.get("command")
        for item in group["hooks"]
        if isinstance(item, dict) and isinstance(item.get("command"), str)
    ]


def test_json_install_is_idempotent_and_uninstall_preserves_existing_hooks(
    tmp_path: Path,
) -> None:
    codex = tmp_path / ".codex"
    codex.mkdir()
    hooks_path = codex / "hooks.json"
    existing = "/usr/local/bin/existing-observer"
    project = "/usr/local/bin/unoq-codex-matrix-hook-test"
    hooks_path.write_text(
        json.dumps(
            {
                "hooks": {
                    "PreToolUse": [
                        {"hooks": [{"type": "command", "command": existing, "timeout": 2}]}
                    ]
                }
            }
        ),
        encoding="utf-8",
    )

    first = _run(INSTALL, tmp_path, project)
    second = _run(INSTALL, tmp_path, project)
    assert first.returncode == second.returncode == 0
    assert "Updated:" in first.stdout
    assert "Already configured:" in second.stdout

    installed = json.loads(hooks_path.read_text(encoding="utf-8"))["hooks"]
    assert set(installed) == EVENTS
    assert existing in _commands(installed["PreToolUse"][0])
    for event in EVENTS:
        occurrences = sum(_commands(group).count(project) for group in installed[event])
        assert occurrences == 1, event

    removed = _run(UNINSTALL, tmp_path, project)
    assert removed.returncode == 0
    remaining = json.loads(hooks_path.read_text(encoding="utf-8"))["hooks"]
    assert set(remaining) == {"PreToolUse"}
    assert _commands(remaining["PreToolUse"][0]) == [existing]


def test_installer_refuses_mixed_json_and_inline_hook_layers(tmp_path: Path) -> None:
    codex = tmp_path / ".codex"
    codex.mkdir()
    (codex / "hooks.json").write_text('{"hooks": {}}\n', encoding="utf-8")
    (codex / "config.toml").write_text(
        '[[hooks.PreToolUse]]\n[[hooks.PreToolUse.hooks]]\ntype = "command"\n'
        'command = "/usr/local/bin/existing"\ntimeout = 1\n',
        encoding="utf-8",
    )

    result = _run(INSTALL, tmp_path, "/usr/local/bin/test-hook")

    assert result.returncode != 0
    assert "mixes hooks.json and inline hooks" in (result.stdout + result.stderr)


def test_hook_trust_metadata_does_not_count_as_inline_lifecycle_hooks(
    tmp_path: Path,
) -> None:
    codex = tmp_path / ".codex"
    codex.mkdir()
    (codex / "config.toml").write_text('[hooks]\nstate = "reviewed"\n', encoding="utf-8")

    result = _run(INSTALL, tmp_path, "/usr/local/bin/test-hook")

    assert result.returncode == 0
    assert (codex / "hooks.json").is_file()
