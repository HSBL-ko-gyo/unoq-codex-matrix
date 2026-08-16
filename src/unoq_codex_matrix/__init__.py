"""UNO Q Codex Matrix core package."""

from .aggregate import AggregateSnapshot, SessionAggregator, SessionStatus
from .events import NormalizedEvent, decode_event, encode_event, normalize_hook_payload
from .protocol import MAX_EVENT_BYTES, PROTOCOL_VERSION, State

__version__ = "0.1.0a1"

__all__ = [
    "AggregateSnapshot",
    "MAX_EVENT_BYTES",
    "NormalizedEvent",
    "PROTOCOL_VERSION",
    "SessionAggregator",
    "SessionStatus",
    "State",
    "decode_event",
    "encode_event",
    "normalize_hook_payload",
]
