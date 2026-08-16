from unoq_codex_matrix.aggregate import SessionAggregator
from unoq_codex_matrix.events import NormalizedEvent
from unoq_codex_matrix.protocol import State


SECOND = 1_000_000_000


def event(
    session: str,
    category: str,
    state: State,
    timestamp: int,
    *,
    turn: str = "turn-1",
    tool_use: str = "",
    failed: bool = False,
) -> NormalizedEvent:
    return NormalizedEvent(
        event=category,
        session=session,
        turn=turn,
        tool_use=tool_use,
        tool="Bash" if tool_use else "",
        state=state,
        failed=failed,
        time_ns=timestamp,
    )


def test_multiple_sessions_use_priority_then_recency() -> None:
    aggregator = SessionAggregator()
    aggregator.process(event("one", "pre_tool", State.WRITING, 10), now_ns=10)
    aggregator.process(event("two", "pre_tool", State.TESTING, 20), now_ns=20)
    aggregator.process(event("three", "permission", State.WAITING, 30), now_ns=30)

    snapshot = aggregator.snapshot(now_ns=31)
    assert snapshot.state is State.WAITING
    assert snapshot.active_sessions == 3

    # Same rank: the most recently updated session wins.
    aggregator.process(event("one", "permission", State.WAITING, 40), now_ns=40)
    assert aggregator.snapshot(now_ns=41).updated_monotonic_ns == 40


def test_active_work_beats_another_sessions_success() -> None:
    aggregator = SessionAggregator()
    aggregator.process(event("done", "stop", State.SUCCESS, 1), now_ns=1)
    aggregator.process(event("busy", "user_prompt", State.THINKING, 2), now_ns=2)
    assert aggregator.snapshot(now_ns=3).state is State.THINKING


def test_success_holds_then_returns_to_idle() -> None:
    aggregator = SessionAggregator(success_hold_s=8)
    aggregator.process(event("one", "stop", State.SUCCESS, SECOND), now_ns=SECOND)

    assert aggregator.snapshot(now_ns=9 * SECOND - 1).state is State.SUCCESS
    assert aggregator.snapshot(now_ns=9 * SECOND).state is State.IDLE
    session = aggregator.get_session("one")
    assert session is not None
    assert session.previous_state is State.SUCCESS
    assert session.completion_expiry is None


def test_transient_error_returns_to_thinking() -> None:
    aggregator = SessionAggregator(transient_error_s=1.5)
    aggregator.process(
        event(
            "one",
            "post_tool",
            State.ERROR,
            SECOND,
            tool_use="item-1",
            failed=True,
        ),
        now_ns=SECOND,
    )

    assert aggregator.snapshot(now_ns=int(2.5 * SECOND) - 1).state is State.ERROR
    assert aggregator.snapshot(now_ns=int(2.5 * SECOND)).state is State.THINKING
    session = aggregator.get_session("one")
    assert session is not None
    assert session.transient_error_expiry is None


def test_session_end_removes_only_that_session() -> None:
    aggregator = SessionAggregator()
    aggregator.process(event("one", "session_start", State.IDLE, 1), now_ns=1)
    aggregator.process(event("two", "user_prompt", State.THINKING, 2), now_ns=2)
    assert aggregator.process(event("two", "session_end", State.IDLE, 3), now_ns=3)
    assert set(aggregator.sessions) == {"one"}
    assert aggregator.snapshot(now_ns=4).active_sessions == 1


def test_session_end_after_stop_keeps_content_free_success_lease() -> None:
    aggregator = SessionAggregator(success_hold_s=8)
    aggregator.process(event("done", "stop", State.SUCCESS, SECOND), now_ns=SECOND)
    aggregator.process(
        event("done", "session_end", State.IDLE, SECOND + 1), now_ns=SECOND + 1
    )

    snapshot = aggregator.snapshot(now_ns=2 * SECOND)
    assert snapshot.state is State.SUCCESS
    assert snapshot.active_sessions == 0
    assert aggregator.sessions == {}
    assert aggregator.snapshot(now_ns=9 * SECOND).state is State.IDLE


def test_active_work_beats_detached_success_lease() -> None:
    aggregator = SessionAggregator(success_hold_s=8)
    aggregator.process(event("done", "stop", State.SUCCESS, SECOND), now_ns=SECOND)
    aggregator.process(
        event("done", "session_end", State.IDLE, SECOND + 1), now_ns=SECOND + 1
    )
    aggregator.process(
        event("busy", "user_prompt", State.THINKING, SECOND + 2), now_ns=SECOND + 2
    )
    assert aggregator.snapshot(now_ns=SECOND + 3).state is State.THINKING


def test_stale_sessions_are_discarded() -> None:
    aggregator = SessionAggregator(stale_session_s=5)
    aggregator.process(event("old", "user_prompt", State.THINKING, SECOND), now_ns=SECOND)
    assert aggregator.snapshot(now_ns=6 * SECOND - 1).active_sessions == 1
    assert aggregator.snapshot(now_ns=6 * SECOND).active_sessions == 0


def test_duplicate_key_is_applied_once() -> None:
    aggregator = SessionAggregator()
    original = event(
        "one", "pre_tool", State.TESTING, 100, turn="turn-1", tool_use="item-1"
    )
    duplicate_with_new_timestamp = event(
        "one", "pre_tool", State.TESTING, 200, turn="turn-1", tool_use="item-1"
    )

    assert aggregator.process(original, now_ns=100)
    assert not aggregator.process(duplicate_with_new_timestamp, now_ns=200)
    assert aggregator.get_session("one").updated_monotonic_ns == 100  # type: ignore[union-attr]


def test_out_of_order_event_cannot_roll_state_back() -> None:
    aggregator = SessionAggregator()
    aggregator.process(
        event("one", "pre_tool", State.TESTING, 200, tool_use="new-item"), now_ns=200
    )
    changed = aggregator.process(
        event("one", "user_prompt", State.THINKING, 100, turn="older-turn"), now_ns=201
    )

    assert not changed
    assert aggregator.snapshot(now_ns=202).state is State.TESTING
    assert aggregator.get_session("one").turn_id == "turn-1"  # type: ignore[union-attr]


def test_delayed_old_session_end_does_not_remove_active_session() -> None:
    aggregator = SessionAggregator()
    aggregator.process(event("one", "user_prompt", State.THINKING, 200), now_ns=200)
    assert not aggregator.process(event("one", "session_end", State.IDLE, 100), now_ns=201)
    assert aggregator.snapshot(now_ns=202).active_sessions == 1


def test_delayed_event_does_not_cancel_success_lease() -> None:
    aggregator = SessionAggregator(success_hold_s=8)
    aggregator.process(event("one", "stop", State.SUCCESS, 200), now_ns=200)

    changed = aggregator.process(
        event("one", "post_tool", State.THINKING, 100, tool_use="old-item"),
        now_ns=201,
    )

    assert not changed
    assert aggregator.snapshot(now_ns=202).state is State.SUCCESS


def test_delayed_event_cannot_resurrect_ended_session() -> None:
    aggregator = SessionAggregator(success_hold_s=1, stale_session_s=10)
    aggregator.process(event("one", "stop", State.SUCCESS, SECOND), now_ns=SECOND)
    aggregator.process(
        event("one", "session_end", State.IDLE, SECOND + 1), now_ns=SECOND + 1
    )

    # The completion lease has expired, but the bounded SessionEnd tombstone
    # still rejects an older datagram from the same session.
    changed = aggregator.process(
        event(
            "one",
            "post_tool",
            State.THINKING,
            SECOND - 1,
            tool_use="delayed-item",
        ),
        now_ns=3 * SECOND,
    )

    assert not changed
    assert aggregator.snapshot(now_ns=3 * SECOND + 1).active_sessions == 0


def test_session_end_arriving_first_blocks_older_session_event() -> None:
    aggregator = SessionAggregator()

    assert aggregator.process(
        event("ended-first", "session_end", State.IDLE, 2_000), now_ns=3_000
    )
    assert not aggregator.process(
        event("ended-first", "user_prompt", State.THINKING, 1_000), now_ns=4_000
    )
    snapshot = aggregator.snapshot(now_ns=5_000)
    assert snapshot.state is State.IDLE
    assert snapshot.active_sessions == 0


def test_error_has_global_precedence_over_waiting_and_flashing() -> None:
    aggregator = SessionAggregator(transient_error_s=10)
    aggregator.process(event("flash", "pre_tool", State.FLASHING, 1), now_ns=1)
    aggregator.process(event("wait", "permission", State.WAITING, 2), now_ns=2)
    aggregator.process(
        event("err", "post_tool", State.ERROR, 3, tool_use="x", failed=True), now_ns=3
    )
    assert aggregator.snapshot(now_ns=4).state is State.ERROR
