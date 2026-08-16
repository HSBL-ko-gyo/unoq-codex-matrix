import json

import pytest

from unoq_codex_matrix.events import (
    EVENT_CATEGORIES,
    WIRE_FIELDS,
    encode_event,
    normalize_hook_payload,
    parse_hook_json,
)
from unoq_codex_matrix.protocol import State


@pytest.mark.parametrize(
    ("hook_name", "expected_event", "expected_state"),
    [
        ("SessionStart", "session_start", State.IDLE),
        ("UserPromptSubmit", "user_prompt", State.THINKING),
        ("PermissionRequest", "permission", State.WAITING),
        ("PreCompact", "pre_compact", State.THINKING),
        ("PostCompact", "post_compact", State.THINKING),
        ("SubagentStart", "subagent_start", State.SUBAGENT),
        ("SubagentStop", "subagent_stop", State.THINKING),
        ("Stop", "stop", State.SUCCESS),
        ("SessionEnd", "session_end", State.IDLE),
    ],
)
def test_lifecycle_event_normalization(
    hook_name: str, expected_event: str, expected_state: State
) -> None:
    event = normalize_hook_payload(
        {
            "hook_event_name": hook_name,
            "session_id": "thr_123",
            "turn_id": "turn_456",
            "prompt": "must never leave the hook process",
            "last_assistant_message": "also private",
            "transcript_path": "/private/transcript.jsonl",
            "cwd": "/private/project",
        },
        time_ns=999,
    )

    assert event.event == expected_event
    assert event.state is expected_state
    assert event.session == "thr_123"
    assert event.turn == "turn_456"
    assert event.time_ns == 999
    assert event.tool == ""
    assert event.failed is False


def test_pre_tool_normalization_classifies_then_discards_input() -> None:
    payload = {
        "hook_event_name": "PreToolUse",
        "session_id": "thr_1",
        "turn_id": "turn_1",
        "tool_use_id": "item_1",
        "tool_name": "Bash",
        "tool_input": {
            "command": "python -m pytest /private/project --token never-store-this"
        },
        "environment": {"API_KEY": "never-store-this"},
    }
    event = normalize_hook_payload(payload, time_ns=100)
    wire = encode_event(event)

    assert event.event == "pre_tool"
    assert event.state is State.TESTING
    assert event.tool == "Bash"
    assert event.tool_use == "item_1"
    assert set(json.loads(wire)) == WIRE_FIELDS
    assert b"pytest" not in wire
    assert b"private" not in wire
    assert b"never-store" not in wire


@pytest.mark.parametrize(
    ("response", "failed", "state"),
    [
        ({"exit_code": 0, "output": "contains the word error"}, False, State.THINKING),
        ({"exit_code": 2, "output": "private body"}, True, State.ERROR),
        ({"status": "failed"}, True, State.ERROR),
        ({"success": False}, True, State.ERROR),
        ("error: a free-form response is not explicit metadata", False, State.THINKING),
    ],
)
def test_post_tool_explicit_failure_only(response: object, failed: bool, state: State) -> None:
    event = normalize_hook_payload(
        {
            "hook_event_name": "PostToolUse",
            "session_id": "thr_1",
            "turn_id": "turn_1",
            "tool_use_id": "item_1",
            "tool_name": "Bash",
            "tool_response": response,
        },
        time_ns=101,
    )
    assert event.failed is failed
    assert event.state is state


def test_parse_hook_json_accepts_schema_aliases() -> None:
    event = parse_hook_json(
        b'{"hookEventName":"UserPromptSubmit","thread_id":"thr_a","turnId":"turn_b"}',
        time_ns=7,
    )
    assert event.event == "user_prompt"
    assert event.session == "thr_a"
    assert event.turn == "turn_b"
    assert EVENT_CATEGORIES == {
        "session_start",
        "user_prompt",
        "pre_tool",
        "permission",
        "post_tool",
        "pre_compact",
        "post_compact",
        "subagent_start",
        "subagent_stop",
        "stop",
        "session_end",
    }
