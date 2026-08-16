"""Strict normalization for Codex lifecycle hook events.

Only identifiers and coarse classifications cross the Unix datagram boundary.
Prompts, assistant text, commands, tool responses, paths, and environment data
are never present in :class:`NormalizedEvent`.
"""

from __future__ import annotations

import json
import time
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Final

from .classify import classify_tool, explicit_failure
from .protocol import MAX_EVENT_BYTES, PROTOCOL_VERSION, State


class EventError(ValueError):
    """Base class for malformed or unsupported event input."""


class OversizedEventError(EventError):
    """The hook input or normalized datagram exceeded the protocol limit."""


class UnsupportedVersionError(EventError):
    """The normalized datagram uses an unsupported protocol version."""


class UnknownEventError(EventError):
    """The hook/datagram event name is not part of protocol v1."""


EVENT_NAME_MAP: Final = {
    "sessionstart": "session_start",
    "userpromptsubmit": "user_prompt",
    "pretooluse": "pre_tool",
    "permissionrequest": "permission",
    "posttooluse": "post_tool",
    "precompact": "pre_compact",
    "postcompact": "post_compact",
    "subagentstart": "subagent_start",
    "subagentstop": "subagent_stop",
    "stop": "stop",
    "sessionend": "session_end",
}
EVENT_CATEGORIES: Final = frozenset(EVENT_NAME_MAP.values())
WIRE_FIELDS: Final = frozenset(
    {"v", "event", "session", "turn", "tool_use", "tool", "state", "failed", "time_ns"}
)

_STATE_BY_EVENT: Final = {
    "session_start": State.IDLE,
    "user_prompt": State.THINKING,
    "permission": State.WAITING,
    "post_tool": State.THINKING,
    "pre_compact": State.THINKING,
    "post_compact": State.THINKING,
    "subagent_start": State.SUBAGENT,
    "subagent_stop": State.THINKING,
    "stop": State.SUCCESS,
    "session_end": State.IDLE,
}


def _bounded_text(value: object, field: str, maximum: int, *, required: bool = False) -> str:
    if value is None and not required:
        return ""
    if not isinstance(value, str):
        raise EventError(f"{field} must be a string")
    if required and not value:
        raise EventError(f"{field} is required")
    if "\x00" in value or len(value) > maximum:
        raise EventError(f"invalid {field}")
    return value


def _first(payload: Mapping[str, object], *keys: str) -> object:
    for key in keys:
        if key in payload:
            return payload[key]
    return None


def _canonical_hook_name(value: object) -> str:
    if not isinstance(value, str):
        raise EventError("hook_event_name is required")
    compact = "".join(character for character in value.lower() if character.isalnum())
    try:
        return EVENT_NAME_MAP[compact]
    except KeyError as exc:
        raise UnknownEventError(f"unknown hook event: {value!r}") from exc


@dataclass(frozen=True, slots=True)
class NormalizedEvent:
    """The complete, privacy-reviewed v1 Unix datagram payload."""

    event: str
    session: str
    state: State
    time_ns: int
    turn: str = ""
    tool_use: str = ""
    tool: str = ""
    failed: bool = False
    v: int = PROTOCOL_VERSION

    def __post_init__(self) -> None:
        if self.v != PROTOCOL_VERSION:
            raise UnsupportedVersionError(f"unsupported protocol version: {self.v!r}")
        if self.event not in EVENT_CATEGORIES:
            raise UnknownEventError(f"unknown normalized event: {self.event!r}")
        _bounded_text(self.session, "session", 256, required=True)
        _bounded_text(self.turn, "turn", 256)
        _bounded_text(self.tool_use, "tool_use", 256)
        _bounded_text(self.tool, "tool", 128)
        if isinstance(self.time_ns, bool) or not isinstance(self.time_ns, int) or self.time_ns < 0:
            raise EventError("time_ns must be a non-negative integer")
        if not isinstance(self.state, State):
            object.__setattr__(self, "state", State.parse(self.state))
        if not isinstance(self.failed, bool):
            raise EventError("failed must be a boolean")

    @property
    def dedup_key(self) -> tuple[str, str, str, str]:
        return self.session, self.turn, self.tool_use, self.event

    def to_wire_dict(self) -> dict[str, object]:
        return {
            "v": self.v,
            "event": self.event,
            "session": self.session,
            "turn": self.turn,
            "tool_use": self.tool_use,
            "tool": self.tool,
            "state": self.state.wire_name,
            "failed": self.failed,
            "time_ns": self.time_ns,
        }


def normalize_hook_payload(
    payload: Mapping[str, object], *, time_ns: int | None = None
) -> NormalizedEvent:
    """Normalize one decoded Codex hook object.

    The returned type has no fields capable of carrying command or response
    bodies.  ``time_ns`` defaults to this machine's monotonic clock and any wall
    clock or timestamp supplied by Codex is ignored.
    """

    if not isinstance(payload, Mapping):
        raise EventError("hook payload must be an object")
    event = _canonical_hook_name(
        _first(payload, "hook_event_name", "event_name", "hookEventName", "event")
    )
    session = _bounded_text(
        _first(payload, "session_id", "sessionId", "conversation_id", "thread_id", "session"),
        "session",
        256,
        required=True,
    )
    turn = _bounded_text(_first(payload, "turn_id", "turnId", "turn"), "turn", 256)
    tool_use = _bounded_text(
        _first(payload, "tool_use_id", "toolUseId", "item_id", "tool_use"), "tool_use", 256
    )
    tool = _bounded_text(_first(payload, "tool_name", "toolName", "tool"), "tool", 128)

    failed = False
    if event == "pre_tool":
        tool_input = _first(payload, "tool_input", "toolInput", "input")
        state = classify_tool(tool, tool_input)
    elif event == "post_tool":
        response = _first(payload, "tool_response", "toolResponse", "tool_result", "response")
        top_level_failure = {
            key: payload[key]
            for key in (
                "exit_code",
                "exitCode",
                "return_code",
                "returnCode",
                "status",
                "success",
                "is_error",
                "isError",
                "error",
                "tool_error",
                "toolError",
            )
            if key in payload
        }
        failed = explicit_failure(response) or explicit_failure(top_level_failure)
        state = State.ERROR if failed else State.THINKING
    else:
        state = _STATE_BY_EVENT[event]

    observed_ns = time.monotonic_ns() if time_ns is None else time_ns
    return NormalizedEvent(
        event=event,
        session=session,
        turn=turn,
        tool_use=tool_use,
        tool=tool if event in {"pre_tool", "post_tool"} else "",
        state=state,
        failed=failed,
        time_ns=observed_ns,
    )


def _reject_constant(value: str) -> object:
    raise EventError(f"non-finite JSON number is not allowed: {value}")


def parse_hook_json(data: bytes | bytearray | memoryview | str, *, time_ns: int | None = None) -> NormalizedEvent:
    """Strictly decode and normalize hook stdin, enforcing the 4 KiB limit."""

    if isinstance(data, str):
        encoded = data.encode("utf-8")
    elif isinstance(data, (bytes, bytearray, memoryview)):
        encoded = bytes(data)
    else:
        raise EventError("hook input must be bytes or text")
    if len(encoded) > MAX_EVENT_BYTES:
        raise OversizedEventError(f"hook input exceeds {MAX_EVENT_BYTES} bytes")
    try:
        payload = json.loads(encoded.decode("utf-8"), parse_constant=_reject_constant)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EventError("invalid hook JSON") from exc
    if not isinstance(payload, Mapping):
        raise EventError("hook JSON must contain an object")
    return normalize_hook_payload(payload, time_ns=time_ns)


def safe_parse_hook_json(
    data: bytes | bytearray | memoryview | str, *, time_ns: int | None = None
) -> NormalizedEvent | None:
    """Fail-open wrapper intended for the short-lived hook executable."""

    try:
        return parse_hook_json(data, time_ns=time_ns)
    except Exception:
        return None


def encode_event(event: NormalizedEvent) -> bytes:
    """Encode a normalized event to a compact, bounded JSON datagram."""

    if not isinstance(event, NormalizedEvent):
        raise EventError("event must be a NormalizedEvent")
    encoded = json.dumps(event.to_wire_dict(), separators=(",", ":"), ensure_ascii=True).encode("ascii")
    if len(encoded) > MAX_EVENT_BYTES:
        raise OversizedEventError(f"normalized event exceeds {MAX_EVENT_BYTES} bytes")
    return encoded


def decode_event(data: bytes | bytearray | memoryview) -> NormalizedEvent:
    """Validate an untrusted datagram received by the daemon."""

    if not isinstance(data, (bytes, bytearray, memoryview)):
        raise EventError("datagram must be bytes")
    encoded = bytes(data)
    if len(encoded) > MAX_EVENT_BYTES:
        raise OversizedEventError(f"datagram exceeds {MAX_EVENT_BYTES} bytes")
    try:
        payload = json.loads(encoded.decode("utf-8"), parse_constant=_reject_constant)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EventError("invalid event JSON") from exc
    if not isinstance(payload, dict):
        raise EventError("event datagram must contain an object")
    unknown_fields = set(payload) - WIRE_FIELDS
    if unknown_fields:
        raise EventError("event datagram contains fields outside the privacy schema")

    version = payload.get("v")
    if isinstance(version, bool) or version != PROTOCOL_VERSION:
        raise UnsupportedVersionError(f"unsupported protocol version: {version!r}")
    event_name = payload.get("event")
    if event_name not in EVENT_CATEGORIES:
        raise UnknownEventError(f"unknown normalized event: {event_name!r}")
    if "state" not in payload:
        raise EventError("state is required")
    try:
        state = State.parse(payload["state"])
    except ValueError as exc:
        raise EventError("invalid state") from exc
    failed = payload.get("failed")
    if not isinstance(failed, bool):
        raise EventError("failed must be a boolean")
    timestamp = payload.get("time_ns")
    if isinstance(timestamp, bool) or not isinstance(timestamp, int) or timestamp < 0:
        raise EventError("time_ns must be a non-negative integer")

    return NormalizedEvent(
        v=version,
        event=event_name,
        session=_bounded_text(payload.get("session"), "session", 256, required=True),
        turn=_bounded_text(payload.get("turn"), "turn", 256),
        tool_use=_bounded_text(payload.get("tool_use"), "tool_use", 256),
        tool=_bounded_text(payload.get("tool"), "tool", 128),
        state=state,
        failed=failed,
        time_ns=timestamp,
    )


# Concise aliases for callers that describe the wire object simply as Event.
Event = NormalizedEvent
parse_datagram = decode_event
