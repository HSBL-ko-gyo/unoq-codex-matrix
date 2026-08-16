from __future__ import annotations

import json
from pathlib import Path


FORBIDDEN = {
    "prompt",
    "last_assistant_message",
    "tool_input",
    "tool_response",
    "transcript_path",
    "cwd",
    "environment",
    "api_key",
    "password",
    "token",
}
PROBE_FIELDS = {
    "hook_event_name",
    "tool_name",
    "session_prefix_hash",
    "turn_prefix_hash",
    "event_time_ns",
    "tool_response_type",
}


def test_json_hook_fixtures_contain_no_raw_private_fields() -> None:
    root = Path(__file__).parent / "fixtures" / "hooks"
    for path in root.glob("*.json"):
        data = json.loads(path.read_text(encoding="utf-8"))
        serialized = json.dumps(data).lower()
        for forbidden in FORBIDDEN:
            assert f'"{forbidden}"' not in serialized, (path, forbidden)


def test_jsonl_hook_fixtures_match_the_probe_allowlist() -> None:
    root = Path(__file__).parent / "fixtures" / "hooks"
    for path in root.glob("*.jsonl"):
        for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            record = json.loads(line)
            assert isinstance(record, dict), (path, line_number)
            assert set(record) == PROBE_FIELDS, (path, line_number, set(record))
            serialized = json.dumps(record).lower()
            for forbidden in FORBIDDEN - {"tool_response"}:
                assert f'"{forbidden}"' not in serialized, (path, line_number, forbidden)
