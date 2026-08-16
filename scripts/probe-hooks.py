#!/usr/bin/env python3
"""Privacy-safe lifecycle schema probe used during hardware qualification.

This is deliberately separate from the production hook.  It records only the
allowlisted metadata needed to document event availability and response types.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
from typing import Any


MAX_INPUT = 65_536


def short_hash(value: Any) -> str | None:
    if not isinstance(value, str) or not value:
        return None
    return hashlib.sha256(value[:8].encode("utf-8")).hexdigest()[:12]


def type_name(value: Any) -> str | None:
    if value is None:
        return None
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, dict):
        return "object"
    if isinstance(value, list):
        return "array"
    if isinstance(value, str):
        return "string"
    if isinstance(value, (int, float)):
        return "number"
    return "unknown"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        raw = sys.stdin.buffer.read(MAX_INPUT + 1)
        if len(raw) > MAX_INPUT:
            return 0
        payload = json.loads(raw.decode("utf-8"))
        if not isinstance(payload, dict):
            return 0
        event = payload.get("hook_event_name")
        if not isinstance(event, str):
            return 0
        record = {
            "hook_event_name": event,
            "tool_name": payload.get("tool_name")
            if isinstance(payload.get("tool_name"), str)
            else None,
            "session_prefix_hash": short_hash(payload.get("session_id")),
            "turn_prefix_hash": short_hash(payload.get("turn_id")),
            "event_time_ns": __import__("time").time_ns(),
            "tool_response_type": type_name(payload.get("tool_response")),
        }
        args.output.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        descriptor = os.open(
            args.output,
            os.O_WRONLY | os.O_CREAT | os.O_APPEND,
            0o600,
        )
        try:
            os.write(descriptor, json.dumps(record, separators=(",", ":")).encode() + b"\n")
        finally:
            os.close(descriptor)
    except Exception:
        pass
    if isinstance(locals().get("event"), str) and event in {"Stop", "SubagentStop"}:
        try:
            os.write(sys.stdout.fileno(), b"{}")
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
