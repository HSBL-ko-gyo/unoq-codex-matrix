#!/usr/bin/env python3
"""Add this project's command to a user's Codex hook layer safely."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import tempfile
import tomllib
from typing import Any


EVENTS = (
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
)
DEFAULT_COMMAND = "/usr/local/bin/unoq-codex-matrix-hook"
DESCRIPTION = "UNO Q Codex Matrix lifecycle observer"
BEGIN_MARKER = "# BEGIN unoq-codex-matrix-hooks"
END_MARKER = "# END unoq-codex-matrix-hooks"


def timestamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def backup(path: Path, stamp: str) -> Path | None:
    if not path.exists():
        return None
    destination = path.with_name(f"{path.name}.bak.{stamp}")
    shutil.copy2(path, destination)
    return destination


def atomic_text(path: Path, text: str, mode: int | None = None) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    previous_mode = mode
    if previous_mode is None and path.exists():
        previous_mode = path.stat().st_mode & 0o777
    previous_mode = 0o600 if previous_mode is None else previous_mode
    descriptor, name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(name, previous_mode)
        os.replace(name, path)
    finally:
        try:
            os.unlink(name)
        except FileNotFoundError:
            pass


def has_command(value: Any, command: str) -> bool:
    if isinstance(value, dict):
        if value.get("type") == "command" and value.get("command") == command:
            return True
        return any(has_command(child, command) for child in value.values())
    if isinstance(value, list):
        return any(has_command(child, command) for child in value)
    return False


def read_json_hooks(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {"description": DESCRIPTION, "hooks": {}}
    with path.open("rb") as handle:
        data = json.load(handle)
    if not isinstance(data, dict) or not isinstance(data.get("hooks", {}), dict):
        raise ValueError("hooks.json has an invalid structure")
    data.setdefault("hooks", {})
    return data


def install_json(path: Path, command: str) -> bool:
    data = read_json_hooks(path)
    hooks = data["hooks"]
    changed = False
    for event in EVENTS:
        groups = hooks.setdefault(event, [])
        if not isinstance(groups, list):
            raise ValueError(f"hooks.json event {event} is not a list")
        if has_command(groups, command):
            continue
        groups.append(
            {
                "hooks": [
                    {"type": "command", "command": command, "timeout": 1}
                ]
            }
        )
        changed = True
    if changed:
        text = json.dumps(data, indent=2, ensure_ascii=False) + "\n"
        json.loads(text)
        atomic_text(path, text)
    return changed


def inline_missing(config: dict[str, Any], command: str) -> list[str]:
    hooks = config.get("hooks")
    if not isinstance(hooks, dict):
        return list(EVENTS)
    return [event for event in EVENTS if not has_command(hooks.get(event, []), command)]


def install_inline(path: Path, command: str) -> bool:
    existing = path.read_text(encoding="utf-8")
    config = tomllib.loads(existing)
    if BEGIN_MARKER in existing or END_MARKER in existing:
        if existing.count(BEGIN_MARKER) != 1 or existing.count(END_MARKER) != 1:
            raise ValueError("invalid UNO Q hook marker in config.toml")
        return False
    missing = inline_missing(config, command)
    if not missing:
        return False
    lines = ["", BEGIN_MARKER]
    encoded_command = json.dumps(command)
    for event in missing:
        lines.extend(
            (
                f"[[hooks.{event}]]",
                f"[[hooks.{event}.hooks]]",
                'type = "command"',
                f"command = {encoded_command}",
                "timeout = 1",
                "",
            )
        )
    lines.append(END_MARKER)
    updated = existing.rstrip() + "\n" + "\n".join(lines) + "\n"
    tomllib.loads(updated)
    atomic_text(path, updated)
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--home", type=Path, default=Path.home())
    parser.add_argument("--command", default=DEFAULT_COMMAND)
    args = parser.parse_args()
    codex_dir = args.home.expanduser().resolve() / ".codex"
    hooks_path = codex_dir / "hooks.json"
    config_path = codex_dir / "config.toml"

    config: dict[str, Any] = {}
    if config_path.exists():
        with config_path.open("rb") as handle:
            config = tomllib.load(handle)
    hook_table = config.get("hooks")
    # Current Codex stores review/trust metadata under [hooks] (for example a
    # `state` entry).  That is not an inline lifecycle-Hook definition and can
    # safely coexist with the documented user hooks.json layer.
    inline = isinstance(hook_table, dict) and any(event in hook_table for event in EVENTS)
    if hooks_path.exists() and inline:
        raise SystemExit(
            "Refusing to add to a layer that already mixes hooks.json and inline hooks."
        )

    stamp = timestamp()
    backups = [backup(path, stamp) for path in (hooks_path, config_path)]
    if inline:
        changed = install_inline(config_path, args.command)
        target = config_path
    else:
        changed = install_json(hooks_path, args.command)
        target = hooks_path

    # Re-parse after writing before reporting success.
    if target.suffix == ".json":
        read_json_hooks(target)
    else:
        with target.open("rb") as handle:
            tomllib.load(handle)
    feature_hooks = config.get("features", {}).get("hooks") if config else None
    if feature_hooks is False:
        print("Warning: [features].hooks is false; Codex will not run user hooks.")
    for saved in backups:
        if saved is not None:
            print(f"Backup: {saved}")
    print(f"{'Updated' if changed else 'Already configured'}: {target}")
    print("Review and trust the exact command in Codex with /hooks before testing.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
