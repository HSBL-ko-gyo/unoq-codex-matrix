#!/usr/bin/env python3
"""Remove only the hook handlers installed by this project."""

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


DEFAULT_COMMAND = "/usr/local/bin/unoq-codex-matrix-hook"
BEGIN_MARKER = "# BEGIN unoq-codex-matrix-hooks"
END_MARKER = "# END unoq-codex-matrix-hooks"


def backup(path: Path, stamp: str) -> None:
    if path.exists():
        shutil.copy2(path, path.with_name(f"{path.name}.bak.{stamp}"))


def atomic_text(path: Path, text: str) -> None:
    mode = path.stat().st_mode & 0o777
    descriptor, name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(name, mode)
        os.replace(name, path)
    finally:
        try:
            os.unlink(name)
        except FileNotFoundError:
            pass


def remove_handler(value: Any, command: str) -> tuple[Any, bool]:
    if not isinstance(value, dict):
        return value, False
    handlers = value.get("hooks")
    if not isinstance(handlers, list):
        return value, False
    kept = [
        handler
        for handler in handlers
        if not (
            isinstance(handler, dict)
            and handler.get("type") == "command"
            and handler.get("command") == command
        )
    ]
    if len(kept) == len(handlers):
        return value, False
    updated = dict(value)
    updated["hooks"] = kept
    return updated, True


def uninstall_json(path: Path, command: str) -> bool:
    if not path.exists():
        return False
    with path.open("rb") as handle:
        data = json.load(handle)
    if not isinstance(data, dict) or not isinstance(data.get("hooks"), dict):
        raise ValueError("hooks.json has an invalid structure")
    changed = False
    hooks = data["hooks"]
    for event in list(hooks):
        groups = hooks[event]
        if not isinstance(groups, list):
            continue
        updated_groups = []
        for group in groups:
            updated, removed = remove_handler(group, command)
            changed = changed or removed
            if not removed or updated.get("hooks"):
                updated_groups.append(updated)
        if updated_groups:
            hooks[event] = updated_groups
        else:
            del hooks[event]
    if changed:
        text = json.dumps(data, indent=2, ensure_ascii=False) + "\n"
        json.loads(text)
        atomic_text(path, text)
    return changed


def uninstall_inline(path: Path) -> bool:
    if not path.exists():
        return False
    text = path.read_text(encoding="utf-8")
    begin, end = text.find(BEGIN_MARKER), text.find(END_MARKER)
    if begin < 0 and end < 0:
        return False
    if begin < 0 or end < begin:
        raise ValueError("invalid UNO Q hook marker in config.toml")
    end += len(END_MARKER)
    updated = (text[:begin].rstrip() + "\n" + text[end:].lstrip("\r\n")).rstrip() + "\n"
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
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    backup(hooks_path, stamp)
    backup(config_path, stamp)
    changed = uninstall_json(hooks_path, args.command)
    changed = uninstall_inline(config_path) or changed
    print("Removed UNO Q hook entries." if changed else "UNO Q hook entries were absent.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
