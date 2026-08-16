from __future__ import annotations

import json
import math
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import threading
import time

import pytest


ROOT = Path(__file__).parents[1]
HOOK = ROOT / "scripts" / "unoq-codex-matrix-hook"
PAYLOAD = b'{"hook_event_name":"UserPromptSubmit","session_id":"thr_test","turn_id":"turn_test"}'
WIRE_FIELDS = {"v", "event", "session", "turn", "tool_use", "tool", "state", "failed", "time_ns"}
HAS_FAST_HOOK_RUNTIME = (
    sys.platform.startswith("linux")
    and Path("/bin/sh").exists()
    and shutil.which("jq") is not None
    and shutil.which("socat") is not None
)
pytestmark = pytest.mark.skipif(
    not HAS_FAST_HOOK_RUNTIME,
    reason="production hook integration requires Linux, jq, and socat",
)


def _environment(path: str) -> dict[str, str]:
    environment = os.environ.copy()
    environment["UNOQ_CODEX_MATRIX_EVENT_SOCKET"] = path
    return environment


def _run(payload: bytes, path: str) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        ["/bin/sh", str(HOOK)],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=_environment(path),
        timeout=1,
        check=False,
    )


def _run_many(path: str, count: int = 110) -> tuple[list[float], list[subprocess.CompletedProcess[bytes]]]:
    timings: list[float] = []
    results = []
    for _ in range(count):
        started = time.perf_counter()
        result = _run(PAYLOAD, path)
        timings.append((time.perf_counter() - started) * 1000)
        results.append(result)
    return timings[10:], results


def _p95(values: list[float]) -> float:
    return sorted(values)[math.ceil(len(values) * 0.95) - 1]


class DatagramReceiver:
    def __init__(self, path: Path) -> None:
        self.path = str(path)
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.socket.bind(self.path)
        self.socket.settimeout(1)

    def receive(self, payload: bytes) -> tuple[subprocess.CompletedProcess[bytes], dict[str, object]]:
        result = _run(payload, self.path)
        datagram = json.loads(self.socket.recv(4096))
        return result, datagram

    def close(self) -> None:
        self.socket.close()


def test_hook_daemon_absent_is_fast_and_fail_open(tmp_path: Path) -> None:
    timings, results = _run_many(str(tmp_path / "absent.sock"))
    assert all(result.returncode == 0 for result in results)
    assert all(not result.stdout and not result.stderr for result in results)
    assert _p95(timings) < 50


def test_hook_daemon_running_is_fast(tmp_path: Path) -> None:
    path = str(tmp_path / "events.sock")
    server = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    server.bind(path)
    server.settimeout(0.1)
    running = True

    def drain() -> None:
        while running:
            try:
                server.recv(4096)
            except (socket.timeout, OSError):
                pass

    worker = threading.Thread(target=drain, daemon=True)
    worker.start()
    try:
        timings, results = _run_many(path)
    finally:
        running = False
        worker.join(timeout=1)
        server.close()
    assert all(result.returncode == 0 for result in results)
    assert all(not result.stdout and not result.stderr for result in results)
    assert _p95(timings) < 50


def test_stop_and_subagent_stop_return_empty_json_without_daemon(tmp_path: Path) -> None:
    path = str(tmp_path / "absent.sock")
    for event in ("Stop", "SubagentStop"):
        result = _run(
            ('{"hook_event_name":"%s","session_id":"thr_test"}' % event).encode(),
            path,
        )
        assert result.returncode == 0
        assert result.stdout == b"{}"
        assert result.stderr == b""


def test_malformed_and_oversized_input_remain_fail_open(tmp_path: Path) -> None:
    path = str(tmp_path / "absent.sock")
    for payload in (b"not json", b"{" + b"x" * 70_000):
        result = _run(payload, path)
        assert result.returncode == 0
        assert result.stdout == b""
        assert result.stderr == b""

    for event in ("Stop", "SubagentStop"):
        oversized_stop = json.dumps(
            {
                "hook_event_name": event,
                "session_id": "thr_test",
                "padding": "x" * 70_000,
            }
        ).encode()
        result = _run(oversized_stop, path)
        assert result.returncode == 0
        assert result.stdout == b"{}"
        assert result.stderr == b""


def test_normalized_datagram_is_capped_at_4k(tmp_path: Path) -> None:
    receiver = DatagramReceiver(tmp_path / "events.sock")
    receiver.socket.settimeout(0.2)
    try:
        payload = json.dumps(
            {
                "hook_event_name": "PreToolUse",
                "session_id": "\x01" * 256,
                "turn_id": "\x02" * 256,
                "tool_use_id": "\x03" * 256,
                "tool_name": "\x04" * 128,
                "tool_input": {},
            }
        ).encode()
        result = _run(payload, receiver.path)
        with pytest.raises(TimeoutError):
            receiver.socket.recv(4096)
    finally:
        receiver.close()
    assert result.returncode == 0
    assert result.stdout == b""
    assert result.stderr == b""


@pytest.mark.parametrize(
    ("hook_event_name", "event", "state"),
    [
        ("SessionStart", "session_start", "idle"),
        ("UserPromptSubmit", "user_prompt", "thinking"),
        ("PermissionRequest", "permission", "waiting"),
        ("PreCompact", "pre_compact", "thinking"),
        ("PostCompact", "post_compact", "thinking"),
        ("SubagentStart", "subagent_start", "subagent"),
        ("SubagentStop", "subagent_stop", "thinking"),
        ("Stop", "stop", "success"),
        ("SessionEnd", "session_end", "idle"),
    ],
)
def test_production_hook_normalizes_lifecycle_events(
    tmp_path: Path, hook_event_name: str, event: str, state: str
) -> None:
    receiver = DatagramReceiver(tmp_path / "events.sock")
    try:
        payload = json.dumps(
            {
                "hook_event_name": hook_event_name,
                "session_id": "thr_private",
                "turn_id": "turn_private",
                "prompt": "must not cross the socket",
            }
        ).encode()
        result, datagram = receiver.receive(payload)
    finally:
        receiver.close()
    assert result.returncode == 0
    assert result.stderr == b""
    assert result.stdout == (b"{}" if hook_event_name in {"Stop", "SubagentStop"} else b"")
    assert set(datagram) == WIRE_FIELDS
    assert datagram["event"] == event
    assert datagram["state"] == state
    assert "prompt" not in datagram


@pytest.mark.parametrize(
    ("tool", "tool_input", "state"),
    [
        ("apply_patch", {}, "writing"),
        ("Read", {}, "reading"),
        ("spawn_agent", {}, "subagent"),
        ("Bash", {"command": "echo ok"}, "command"),
        ("Bash", {"command": "cd src && python -m pytest -q | tee result"}, "testing"),
        ("Bash", {"command": "npm run 'test'"}, "testing"),
        ("Bash", {"command": '"arduino-cli" upload -b arduino:zephyr:unoq'}, "flashing"),
        ("Bash", {"command": "cmake --build build && ninja"}, "building"),
        ("Bash", {"command": "echo 'pytest && make'"}, "command"),
        ("Bash", {"command": "pytest 'unterminated"}, "command"),
    ],
)
def test_production_hook_classifies_pre_tool(
    tmp_path: Path, tool: str, tool_input: object, state: str
) -> None:
    receiver = DatagramReceiver(tmp_path / "events.sock")
    try:
        payload = json.dumps(
            {
                "hook_event_name": "PreToolUse",
                "session_id": "thr_test",
                "turn_id": "turn_test",
                "tool_use_id": "item_test",
                "tool_name": tool,
                "tool_input": tool_input,
            }
        ).encode()
        result, datagram = receiver.receive(payload)
    finally:
        receiver.close()
    assert result.returncode == 0
    assert result.stdout == b""
    assert result.stderr == b""
    assert datagram["state"] == state


@pytest.mark.parametrize(
    ("response", "state", "failed"),
    [
        ({"exit_code": 2, "output": "private"}, "error", True),
        ({"status": "failed"}, "error", True),
        ({"status": " FAILED "}, "error", True),
        ({"success": False}, "error", True),
        ({"content": "the word error is not structured failure"}, "thinking", False),
    ],
)
def test_production_hook_uses_only_explicit_post_tool_failure(
    tmp_path: Path, response: object, state: str, failed: bool
) -> None:
    receiver = DatagramReceiver(tmp_path / "events.sock")
    try:
        payload = json.dumps(
            {
                "hook_event_name": "PostToolUse",
                "session_id": "thr_test",
                "tool_name": "Bash",
                "tool_response": response,
            }
        ).encode()
        _, datagram = receiver.receive(payload)
    finally:
        receiver.close()
    assert datagram["state"] == state
    assert datagram["failed"] is failed
    assert set(datagram) == WIRE_FIELDS
