import json

import pytest

from unoq_codex_matrix.events import (
    EventError,
    OversizedEventError,
    UnknownEventError,
    UnsupportedVersionError,
    decode_event,
    normalize_hook_payload,
    parse_hook_json,
    safe_parse_hook_json,
)
from unoq_codex_matrix.protocol import MAX_EVENT_BYTES, State


@pytest.mark.parametrize(
    "raw",
    [
        b"{not json",
        b"[]",
        b"null",
        b"\xff",
        b'{"hook_event_name": NaN}',
        b'{"hook_event_name":"Stop"}',
    ],
)
def test_malformed_hook_input_is_rejected_and_safe_wrapper_fails_open(raw: bytes) -> None:
    with pytest.raises(EventError):
        parse_hook_json(raw, time_ns=1)
    assert safe_parse_hook_json(raw, time_ns=1) is None


def test_hook_input_has_a_hard_four_kibibyte_limit() -> None:
    raw = b"{" + b" " * MAX_EVENT_BYTES
    assert len(raw) == MAX_EVENT_BYTES + 1
    with pytest.raises(OversizedEventError):
        parse_hook_json(raw, time_ns=1)
    assert safe_parse_hook_json(raw, time_ns=1) is None


def valid_wire() -> dict[str, object]:
    return {
        "v": 1,
        "event": "user_prompt",
        "session": "thr_1",
        "turn": "turn_1",
        "tool_use": "",
        "tool": "",
        "state": "thinking",
        "failed": False,
        "time_ns": 1,
    }


def encoded(payload: dict[str, object]) -> bytes:
    return json.dumps(payload, separators=(",", ":")).encode()


def test_oversized_datagram_is_rejected_before_json_decode() -> None:
    with pytest.raises(OversizedEventError):
        decode_event(b"x" * (MAX_EVENT_BYTES + 1))


def test_unknown_protocol_version_is_rejected() -> None:
    payload = valid_wire()
    payload["v"] = 2
    with pytest.raises(UnsupportedVersionError):
        decode_event(encoded(payload))


def test_unknown_event_is_rejected() -> None:
    payload = valid_wire()
    payload["event"] = "future_event"
    with pytest.raises(UnknownEventError):
        decode_event(encoded(payload))


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("state", "future_state"),
        ("failed", "false"),
        ("time_ns", -1),
        ("session", ""),
        ("tool", "x" * 129),
    ],
)
def test_invalid_wire_field_is_rejected(field: str, value: object) -> None:
    payload = valid_wire()
    payload[field] = value
    with pytest.raises(EventError):
        decode_event(encoded(payload))


@pytest.mark.parametrize(
    "private_field",
    ["prompt", "command", "tool_response", "reasoning", "transcript_path", "cwd", "environment"],
)
def test_datagram_rejects_fields_outside_privacy_schema(private_field: str) -> None:
    payload = valid_wire()
    payload[private_field] = "must not cross the socket"
    with pytest.raises(EventError, match="privacy schema"):
        decode_event(encoded(payload))


def test_unknown_hook_event_is_dropped_by_fail_open_path() -> None:
    raw = b'{"hook_event_name":"FutureEvent","session_id":"thr_1"}'
    with pytest.raises(UnknownEventError):
        parse_hook_json(raw, time_ns=1)
    assert safe_parse_hook_json(raw, time_ns=1) is None


@pytest.mark.parametrize(
    "malformed_response",
    [None, 7, [], "failed with an error", {"output": {"status": "failed"}}],
)
def test_malformed_or_body_only_tool_response_does_not_guess_failure(
    malformed_response: object,
) -> None:
    event = normalize_hook_payload(
        {
            "hook_event_name": "PostToolUse",
            "session_id": "thr_1",
            "tool_response": malformed_response,
        },
        time_ns=1,
    )
    assert event.failed is False
    assert event.state is State.THINKING
