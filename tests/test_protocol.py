import json
import re
from pathlib import Path

import pytest

from unoq_codex_matrix.events import NormalizedEvent, decode_event, encode_event
from unoq_codex_matrix.protocol import (
    MAX_EVENT_BYTES,
    PROTOCOL_VERSION,
    STATE_IDS,
    STATE_NAMES,
    STATE_PRIORITY,
    State,
)


EXPECTED_STATE_IDS = {
    "OFF": 0,
    "IDLE": 1,
    "THINKING": 2,
    "READING": 3,
    "WRITING": 4,
    "COMMAND": 5,
    "BUILDING": 6,
    "TESTING": 7,
    "FLASHING": 8,
    "WAITING": 9,
    "SUCCESS": 10,
    "ERROR": 11,
    "OFFLINE": 12,
    "SUBAGENT": 13,
}

FIRMWARE_PROTOCOL_HEADER = (
    Path(__file__).resolve().parents[1] / "firmware" / "unoq_codex_matrix" / "protocol.h"
)
NATIVE_HOOK_SOURCE = (
    Path(__file__).resolve().parents[1] / "native" / "unoq-codex-matrix-hook.c"
)


def _firmware_protocol_source() -> str:
    return FIRMWARE_PROTOCOL_HEADER.read_text(encoding="utf-8")


def _cpp_decimal_constant(source: str, name: str) -> int:
    match = re.search(
        rf"\bconstexpr\s+\w+\s+{re.escape(name)}\s*=\s*(\d+)(?:U|UL|ULL)?\s*;",
        source,
    )
    assert match is not None, f"missing decimal firmware constant: {name}"
    return int(match.group(1))


def _cpp_state_ids(source: str) -> dict[str, int]:
    enum_match = re.search(
        r"\benum\s+StateId\s*:\s*uint8_t\s*\{(?P<body>.*?)\};",
        source,
        flags=re.DOTALL,
    )
    assert enum_match is not None, "missing firmware StateId enum"
    return {
        name: int(value)
        for name, value in re.findall(
            r"^\s*([A-Z][A-Z0-9_]*)\s*=\s*(\d+)\s*,\s*$",
            enum_match.group("body"),
            flags=re.MULTILINE,
        )
    }


def test_state_ids_are_fixed_wire_abi() -> None:
    assert PROTOCOL_VERSION == 1
    assert MAX_EVENT_BYTES == 4096
    assert dict(STATE_IDS) == EXPECTED_STATE_IDS
    assert dict(STATE_NAMES) == {value: key.lower() for key, value in EXPECTED_STATE_IDS.items()}
    assert [int(state) for state in State] == list(range(14))


def test_firmware_protocol_constants_match_python_wire_abi() -> None:
    source = _firmware_protocol_source()

    assert _cpp_state_ids(source) == dict(STATE_IDS) == EXPECTED_STATE_IDS
    assert _cpp_decimal_constant(source, "kProtocolVersion") == PROTOCOL_VERSION == 1

    width = _cpp_decimal_constant(source, "kMatrixWidth")
    height = _cpp_decimal_constant(source, "kMatrixHeight")
    assert width * height == 104
    assert re.search(
        r"\bconstexpr\s+uint16_t\s+kPixelCount\s*=\s*"
        r"kMatrixWidth\s*\*\s*kMatrixHeight\s*;",
        source,
    )
    assert re.search(r"\bstatic_assert\s*\(\s*kPixelCount\s*==\s*104\s*,", source)


def test_native_hook_state_ids_match_python_and_firmware() -> None:
    source = NATIVE_HOOK_SOURCE.read_text(encoding="utf-8")
    enum_match = re.search(
        r"typedef\s+enum\s*\{(?P<body>[^}]*)\}\s*MatrixState\s*;",
        source,
    )
    assert enum_match is not None, "missing native MatrixState enum"
    native_ids = {
        name.removeprefix("ST_"): int(value)
        for name, value in re.findall(
            r"^\s*(ST_[A-Z][A-Z0-9_]*)\s*=\s*(\d+)\s*,?\s*$",
            enum_match.group("body"),
            flags=re.MULTILINE,
        )
    }
    assert native_ids == dict(STATE_IDS) == EXPECTED_STATE_IDS


@pytest.mark.parametrize("value", ["testing", "TESTING", 7, "7"])
def test_state_parse_accepts_names_and_ids(value: object) -> None:
    assert State.parse(value) is State.TESTING


@pytest.mark.parametrize("value", [True, -1, 14, "unknown", ""])
def test_state_parse_rejects_invalid_values(value: object) -> None:
    with pytest.raises(ValueError):
        State.parse(value)


def test_global_priority_order_is_protocol_order() -> None:
    ordered = [
        State.ERROR,
        State.WAITING,
        State.FLASHING,
        State.TESTING,
        State.BUILDING,
        State.WRITING,
        State.READING,
        State.COMMAND,
        State.SUBAGENT,
        State.THINKING,
        State.SUCCESS,
        State.IDLE,
    ]
    assert all(
        STATE_PRIORITY[higher] > STATE_PRIORITY[lower]
        for higher, lower in zip(ordered, ordered[1:])
    )


def test_event_wire_round_trip_uses_only_reviewed_fields() -> None:
    event = NormalizedEvent(
        event="pre_tool",
        session="thr_public_identifier",
        turn="turn_1",
        tool_use="item_1",
        tool="Bash",
        state=State.TESTING,
        failed=False,
        time_ns=123,
    )
    encoded = encode_event(event)
    wire = json.loads(encoded)

    assert len(encoded) < MAX_EVENT_BYTES
    assert wire == {
        "v": 1,
        "event": "pre_tool",
        "session": "thr_public_identifier",
        "turn": "turn_1",
        "tool_use": "item_1",
        "tool": "Bash",
        "state": "testing",
        "failed": False,
        "time_ns": 123,
    }
    assert decode_event(encoded) == event
