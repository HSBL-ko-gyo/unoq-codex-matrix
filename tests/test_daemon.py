from __future__ import annotations

import json
from pathlib import Path
import socket
import threading
import time

import pytest

from unoq_codex_matrix.bridge import FirmwareVersion, McuStatus
from unoq_codex_matrix.control import request
from unoq_codex_matrix.daemon import Config, MatrixDaemon, load_config
from unoq_codex_matrix.events import NormalizedEvent, encode_event
from unoq_codex_matrix.protocol import State


class FakeBridge:
    def __init__(self, path: str) -> None:
        self.path = path
        self.last_success_monotonic: float | None = None
        self.connected = True
        self.published: list[tuple[int, int]] = []
        Path(path).touch()

    def publish(self, state: State, count: int, **_: object):
        self.last_success_monotonic = time.monotonic()
        self.published.append((int(state), count))
        return McuStatus(1, int(state), count, 3), FirmwareVersion(1, 0, 1, 0)

    def close(self) -> None:
        self.connected = False


class FailingBridge(FakeBridge):
    def publish(self, state: State, count: int, **_: object):
        from unoq_codex_matrix.bridge import RouterError

        raise RouterError("unavailable")


class RecoveringBridge(FakeBridge):
    def __init__(self, path: str) -> None:
        super().__init__(path)
        self.failures_remaining = 1

    def publish(self, state: State, count: int, **kwargs: object):
        if self.failures_remaining:
            self.failures_remaining -= 1
            from unoq_codex_matrix.bridge import RouterError

            raise RouterError("temporary outage")
        return super().publish(state, count, **kwargs)


def test_invalid_config_fields_fall_back_individually(tmp_path: Path) -> None:
    path = tmp_path / "config.json"
    path.write_text(
        json.dumps(
            {
                "brightness": 99,
                "frame_interval_ms": 75,
                "heartbeat_interval_s": "fast",
                "offline_timeout_s": 12,
                "show_active_count": "yes",
                "log_level": "LOUD",
            }
        ),
        encoding="utf-8",
    )
    config = load_config(str(path))
    assert config.brightness == 3
    assert config.frame_interval_ms == 75
    assert config.heartbeat_interval_s == 3
    assert config.show_active_count is True
    assert config.log_level == "INFO"


@pytest.mark.skipif(not hasattr(socket, "AF_UNIX"), reason="requires Unix sockets")
def test_daemon_survives_bad_datagrams_and_control_disconnect(tmp_path: Path) -> None:
    event_path = str(tmp_path / "events.sock")
    control_path = str(tmp_path / "control.sock")
    router_path = str(tmp_path / "router.sock")
    bridge = FakeBridge(router_path)
    daemon = MatrixDaemon(
        Config(heartbeat_interval_s=2),
        event_path=event_path,
        control_path=control_path,
        bridge=bridge,  # type: ignore[arg-type]
    )
    thread = threading.Thread(target=daemon.run)
    thread.start()
    deadline = time.monotonic() + 2
    while not Path(event_path).exists() and time.monotonic() < deadline:
        time.sleep(0.01)
    client = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    for payload in (b"bad", b"{}", b"x" * 4097):
        client.sendto(payload, event_path)
    client.close()
    disconnected = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    disconnected.connect(control_path)
    disconnected.close()

    now = time.monotonic_ns()
    event = NormalizedEvent(
        event="pre_tool",
        session="thr_test",
        turn="turn_test",
        tool_use="item_test",
        tool="Bash",
        state=State.TESTING,
        failed=False,
        time_ns=now,
    )
    sender = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    sender.sendto(encode_event(event), event_path)
    sender.close()
    deadline = time.monotonic() + 2
    status = {}
    while time.monotonic() < deadline:
        status = request({"action": "status"}, path=control_path)
        if status.get("current_state") == "TESTING":
            break
        time.sleep(0.02)
    daemon.stop()
    thread.join(timeout=2)
    assert not thread.is_alive()
    assert daemon.invalid_events >= 3
    assert status["current_state"] == "TESTING"
    assert status["active_session_count"] == 1
    assert "thr_test" not in json.dumps(status)


def test_1000_state_changes_and_three_sessions(tmp_path: Path) -> None:
    daemon = MatrixDaemon(
        Config(), bridge=FakeBridge(str(tmp_path / "router.sock"))  # type: ignore[arg-type]
    )
    start = time.monotonic_ns()
    for index in range(1000):
        session = f"session-{index % 3}"
        event = NormalizedEvent(
            event="pre_tool",
            session=session,
            turn=f"turn-{index}",
            tool_use=f"item-{index}",
            tool="Bash",
            state=(State.TESTING if index % 2 else State.BUILDING),
            failed=False,
            time_ns=start + index,
        )
        daemon.aggregator.process(event, now_ns=start + index)
    snapshot = daemon.aggregator.snapshot(now_ns=start + 1001)
    assert snapshot.active_sessions == 3
    assert snapshot.state is State.TESTING


def test_status_keeps_real_session_count_during_override(tmp_path: Path) -> None:
    daemon = MatrixDaemon(
        Config(), bridge=FakeBridge(str(tmp_path / "router.sock"))  # type: ignore[arg-type]
    )
    now_ns = time.monotonic_ns()
    daemon.aggregator.process(
        NormalizedEvent(
            event="user_prompt",
            session="private-session",
            turn="private-turn",
            tool_use="",
            tool="",
            state=State.THINKING,
            failed=False,
            time_ns=now_ns,
        ),
        now_ns=now_ns,
    )
    daemon.override_state = State.WAITING
    daemon.override_expiry = time.monotonic() + 10

    status = daemon._status()

    assert status["current_state"] == "WAITING"
    assert status["active_session_count"] == 1
    assert "private-session" not in json.dumps(status)


def test_doctor_does_not_report_stale_cached_mcu_as_healthy(tmp_path: Path) -> None:
    bridge = FailingBridge(str(tmp_path / "router.sock"))
    bridge.last_success_monotonic = time.monotonic()
    daemon = MatrixDaemon(Config(), bridge=bridge)  # type: ignore[arg-type]
    daemon.last_mcu_status = McuStatus(1, int(State.IDLE), 0, 3)
    daemon.last_firmware_version = FirmwareVersion(1, 0, 1, 0)

    result = daemon._doctor()

    assert result["healthy"] is False
    assert result["checks"]["MCU RPC"] is False
    assert result["mcu_protocol_version"] is None
    assert result["firmware_version"] is None
    assert result["router_status"] == "reconnecting"


def test_daemon_retries_after_temporary_router_failure(tmp_path: Path) -> None:
    now = [100.0]
    bridge = RecoveringBridge(str(tmp_path / "router.sock"))
    daemon = MatrixDaemon(
        Config(),
        bridge=bridge,  # type: ignore[arg-type]
        monotonic=lambda: now[0],
        monotonic_ns=lambda: int(now[0] * 1_000_000_000),
    )

    assert daemon._publish(force=True) is False
    assert daemon.last_mcu_status is None
    assert daemon._status()["router_status"] == "reconnecting"

    # Normal loop publishes are rate-limited while the Router is unavailable.
    now[0] = 101.9
    assert daemon._publish() is False
    assert bridge.published == []

    # The daemon remains alive and the next eligible publish restores state.
    now[0] = 102.1
    assert daemon._publish() is True
    assert bridge.published == [(int(State.IDLE), 0)]
    assert daemon.last_mcu_status == McuStatus(1, int(State.IDLE), 0, 3)
    assert daemon._status()["router_status"] == "connected"
