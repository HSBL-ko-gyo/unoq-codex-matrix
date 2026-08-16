"""Per-session state tracking and deterministic global aggregation."""

from __future__ import annotations

import time
from collections import OrderedDict
from dataclasses import dataclass

from .events import NormalizedEvent
from .protocol import STATE_PRIORITY, State


_NS_PER_SECOND = 1_000_000_000


@dataclass(slots=True)
class SessionStatus:
    session_id: str
    turn_id: str
    current_state: State
    previous_state: State
    updated_monotonic_ns: int
    tool_use_id: str = ""
    completion_expiry_ns: int | None = None
    transient_error_expiry_ns: int | None = None

    @property
    def updated_monotonic(self) -> int:
        """Compatibility name matching the architecture documentation."""

        return self.updated_monotonic_ns

    @property
    def completion_expiry(self) -> int | None:
        return self.completion_expiry_ns

    @property
    def transient_error_expiry(self) -> int | None:
        return self.transient_error_expiry_ns


@dataclass(frozen=True, slots=True)
class AggregateSnapshot:
    state: State
    active_sessions: int
    updated_monotonic_ns: int


class SessionAggregator:
    """Aggregate lifecycle events from concurrently active Codex sessions."""

    def __init__(
        self,
        *,
        success_hold_s: float = 8.0,
        transient_error_s: float = 1.5,
        stale_session_s: float = 43_200.0,
        dedup_capacity: int = 8192,
    ) -> None:
        if success_hold_s < 0 or transient_error_s < 0 or stale_session_s <= 0:
            raise ValueError("invalid aggregator duration")
        if dedup_capacity < 1:
            raise ValueError("dedup_capacity must be positive")
        self.success_hold_ns = int(success_hold_s * _NS_PER_SECOND)
        self.transient_error_ns = int(transient_error_s * _NS_PER_SECOND)
        self.stale_session_ns = int(stale_session_s * _NS_PER_SECOND)
        self.dedup_capacity = dedup_capacity
        self.sessions: dict[str, SessionStatus] = {}
        self._seen: OrderedDict[tuple[str, str, str, str], int] = OrderedDict()
        # Codex commonly emits SessionEnd immediately after Stop. Keep only a
        # content-free completion lease so SUCCESS remains visible for its TTL
        # while the ended session itself is removed from active aggregation.
        self._completions: dict[str, tuple[int, int]] = {}
        # Retain only the final monotonic timestamp (plus local receipt time)
        # for ended sessions. This prevents a delayed datagram from resurrecting
        # a session after SessionEnd, including after its SUCCESS lease expires.
        self._ended: dict[str, tuple[int, int]] = {}

    def _remember(self, key: tuple[str, str, str, str], received_ns: int) -> bool:
        if key in self._seen:
            self._seen.move_to_end(key)
            return False
        self._seen[key] = received_ns
        while len(self._seen) > self.dedup_capacity:
            self._seen.popitem(last=False)
        return True

    def _expire(self, now_ns: int) -> None:
        self._completions = {
            session_id: lease
            for session_id, lease in self._completions.items()
            if now_ns < lease[0]
        }
        self._ended = {
            session_id: tombstone
            for session_id, tombstone in self._ended.items()
            if now_ns - tombstone[1] < self.stale_session_ns
        }
        stale = [
            session_id
            for session_id, session in self.sessions.items()
            if now_ns - session.updated_monotonic_ns >= self.stale_session_ns
        ]
        for session_id in stale:
            del self.sessions[session_id]

        for session in self.sessions.values():
            if (
                session.current_state is State.ERROR
                and session.transient_error_expiry_ns is not None
                and now_ns >= session.transient_error_expiry_ns
            ):
                session.previous_state = State.ERROR
                session.current_state = State.THINKING
                session.updated_monotonic_ns = now_ns
                session.transient_error_expiry_ns = None
            if (
                session.current_state is State.SUCCESS
                and session.completion_expiry_ns is not None
                and now_ns >= session.completion_expiry_ns
            ):
                session.previous_state = State.SUCCESS
                session.current_state = State.IDLE
                session.updated_monotonic_ns = now_ns
                session.completion_expiry_ns = None

        seen_cutoff = now_ns - self.stale_session_ns
        while self._seen:
            _, received_ns = next(iter(self._seen.items()))
            if received_ns > seen_cutoff:
                break
            self._seen.popitem(last=False)

    def process(self, event: NormalizedEvent, *, now_ns: int | None = None) -> bool:
        """Apply one event, returning whether it changed tracked state.

        Duplicates are identified using exactly
        ``(session, turn, tool_use, event)``.  A non-duplicate event older than
        the last applied event for that session is ignored, preventing delayed
        datagrams from rolling state backwards.
        """

        if not isinstance(event, NormalizedEvent):
            raise TypeError("event must be a NormalizedEvent")
        observed_ns = time.monotonic_ns() if now_ns is None else now_ns
        if isinstance(observed_ns, bool) or not isinstance(observed_ns, int) or observed_ns < 0:
            raise ValueError("now_ns must be a non-negative integer")
        self._expire(observed_ns)
        if not self._remember(event.dedup_key, observed_ns):
            return False

        session = self.sessions.get(event.session)
        if event.event == "session_end":
            if session is not None:
                if event.time_ns < session.updated_monotonic_ns:
                    return False
                del self.sessions[event.session]
            else:
                ended = self._ended.get(event.session)
                if ended is not None and event.time_ns < ended[0]:
                    return False
                completion = self._completions.get(event.session)
                if completion is not None and event.time_ns < completion[1]:
                    return False
            self._ended[event.session] = (event.time_ns, observed_ns)
            return True

        ended = self._ended.get(event.session)
        if ended is not None:
            if event.time_ns <= ended[0]:
                return False
            del self._ended[event.session]

        if session is None:
            completion = self._completions.get(event.session)
            if completion is not None and event.time_ns < completion[1]:
                return False
            session = SessionStatus(
                session_id=event.session,
                turn_id=event.turn,
                current_state=State.IDLE,
                previous_state=State.IDLE,
                updated_monotonic_ns=event.time_ns,
                tool_use_id=event.tool_use,
            )
            self.sessions[event.session] = session
        elif event.time_ns < session.updated_monotonic_ns:
            return False

        if event.event != "stop":
            self._completions.pop(event.session, None)

        next_state = event.state
        if event.event == "post_tool" and event.failed:
            next_state = State.ERROR

        old_state = session.current_state
        session.previous_state = State.THINKING if next_state is State.ERROR else old_state
        session.current_state = next_state
        session.updated_monotonic_ns = event.time_ns
        if event.turn:
            session.turn_id = event.turn
        if event.tool_use:
            session.tool_use_id = event.tool_use

        session.completion_expiry_ns = None
        session.transient_error_expiry_ns = None
        if next_state is State.SUCCESS:
            session.completion_expiry_ns = observed_ns + self.success_hold_ns
            if self.success_hold_ns > 0:
                self._completions[event.session] = (
                    session.completion_expiry_ns,
                    event.time_ns,
                )
        elif next_state is State.ERROR:
            session.transient_error_expiry_ns = observed_ns + self.transient_error_ns
        return True

    # API aliases used by daemon implementations and tests.
    apply = process
    apply_event = process

    def tick(self, *, now_ns: int | None = None) -> None:
        observed_ns = time.monotonic_ns() if now_ns is None else now_ns
        self._expire(observed_ns)

    def snapshot(self, *, now_ns: int | None = None) -> AggregateSnapshot:
        observed_ns = time.monotonic_ns() if now_ns is None else now_ns
        self._expire(observed_ns)
        if not self.sessions and not self._completions:
            return AggregateSnapshot(State.IDLE, 0, observed_ns)
        session_winner = (
            max(
                self.sessions.values(),
                key=lambda session: (
                    STATE_PRIORITY[session.current_state],
                    session.updated_monotonic_ns,
                ),
            )
            if self.sessions
            else None
        )
        completion_updated = max(
            (updated for _, updated in self._completions.values()), default=-1
        )
        if self._completions and (
            session_winner is None
            or (
                STATE_PRIORITY[State.SUCCESS],
                completion_updated,
            )
            > (
                STATE_PRIORITY[session_winner.current_state],
                session_winner.updated_monotonic_ns,
            )
        ):
            return AggregateSnapshot(State.SUCCESS, len(self.sessions), completion_updated)
        return AggregateSnapshot(
            session_winner.current_state,
            len(self.sessions),
            session_winner.updated_monotonic_ns,
        )

    @property
    def active_session_count(self) -> int:
        return len(self.sessions)

    def get_session(self, session_id: str) -> SessionStatus | None:
        return self.sessions.get(session_id)


# Short alias for callers that prefer the component name used in diagrams.
Aggregator = SessionAggregator
