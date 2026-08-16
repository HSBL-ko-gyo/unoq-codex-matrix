"""Shared protocol constants for the Linux daemon and MCU firmware.

The numeric state identifiers are wire ABI.  Do not reorder or renumber them.
"""

from __future__ import annotations

from enum import IntEnum
from types import MappingProxyType
from typing import Final


PROTOCOL_VERSION: Final = 1
MAX_EVENT_BYTES: Final = 4096


class State(IntEnum):
    """LED matrix states shared by Python and the STM32 firmware."""

    OFF = 0
    IDLE = 1
    THINKING = 2
    READING = 3
    WRITING = 4
    COMMAND = 5
    BUILDING = 6
    TESTING = 7
    FLASHING = 8
    WAITING = 9
    SUCCESS = 10
    ERROR = 11
    OFFLINE = 12
    SUBAGENT = 13

    @classmethod
    def parse(cls, value: object) -> "State":
        """Parse a numeric ID or a case-insensitive symbolic state name."""

        if isinstance(value, bool):
            raise ValueError("a boolean is not a state ID")
        if isinstance(value, int):
            return cls(value)
        if isinstance(value, str):
            candidate = value.strip()
            if not candidate:
                raise ValueError("empty state name")
            if candidate.isascii() and candidate.lstrip("+-").isdigit():
                return cls(int(candidate, 10))
            try:
                return cls[candidate.upper()]
            except KeyError as exc:
                raise ValueError(f"unknown state: {value!r}") from exc
        raise ValueError(f"unsupported state value: {type(value).__name__}")

    @property
    def wire_name(self) -> str:
        return self.name.lower()


STATE_IDS: Final = MappingProxyType({state.name: int(state) for state in State})
STATE_NAMES: Final = MappingProxyType({int(state): state.wire_name for state in State})

# Larger values win when statuses from multiple Codex sessions are aggregated.
# OFFLINE is a daemon/transport condition rather than a session state, but it is
# assigned the highest rank so an explicitly injected transport fault is visible.
STATE_PRIORITY: Final = MappingProxyType(
    {
        State.OFF: -1,
        State.IDLE: 0,
        State.SUCCESS: 10,
        State.THINKING: 20,
        State.SUBAGENT: 30,
        State.COMMAND: 40,
        State.READING: 50,
        State.WRITING: 60,
        State.BUILDING: 70,
        State.TESTING: 80,
        State.FLASHING: 90,
        State.WAITING: 100,
        State.ERROR: 110,
        State.OFFLINE: 120,
    }
)


def state_name(value: State | int | str) -> str:
    """Return the canonical lower-case wire name for *value*."""

    return State.parse(value).wire_name
