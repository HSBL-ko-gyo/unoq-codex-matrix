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
SOURCE = ROOT / "native" / "unoq-codex-matrix-hook.c"
WIRE_FIELDS = {
    "v", "event", "session", "turn", "tool_use", "tool", "state", "failed", "time_ns"
}
PAYLOAD = b'{"hook_event_name":"UserPromptSubmit","session_id":"thr_test","turn_id":"turn_test"}'
CAN_RUN = sys.platform.startswith("linux") and (
    os.environ.get("UNOQ_NATIVE_HOOK") is not None or shutil.which("cc") is not None
)
pytestmark = pytest.mark.skipif(not CAN_RUN, reason="native hook requires Linux and a C compiler")


@pytest.fixture(scope="session")
def native_hook(tmp_path_factory: pytest.TempPathFactory) -> Path:
    configured = os.environ.get("UNOQ_NATIVE_HOOK")
    if configured:
        result = Path(configured)
        if not result.is_file():
            pytest.fail("UNOQ_NATIVE_HOOK does not name a file")
        return result
    result = tmp_path_factory.mktemp("native-hook") / "unoq-codex-matrix-hook"
    completed = subprocess.run(
        [
            "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            str(SOURCE), "-o", str(result),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr.decode(errors="replace")
    return result


def _run(binary: Path, payload: bytes, path: str) -> subprocess.CompletedProcess[bytes]:
    environment = os.environ.copy()
    environment["UNOQ_CODEX_MATRIX_EVENT_SOCKET"] = path
    return subprocess.run(
        [str(binary)],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
        timeout=1,
        check=False,
    )


class Receiver:
    def __init__(self, path: Path) -> None:
        self.path = str(path)
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.socket.bind(self.path)
        self.socket.settimeout(0.5)

    def invoke(self, binary: Path, payload: object) -> tuple[subprocess.CompletedProcess[bytes], dict]:
        encoded = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
        result = _run(binary, encoded, self.path)
        datagram = json.loads(self.socket.recv(4096))
        return result, datagram

    def close(self) -> None:
        self.socket.close()


@pytest.mark.parametrize(
    ("hook_name", "event", "state"),
    [
        ("SessionStart", "session_start", "idle"),
        ("UserPromptSubmit", "user_prompt", "thinking"),
        ("PreToolUse", "pre_tool", "command"),
        ("PermissionRequest", "permission", "waiting"),
        ("PostToolUse", "post_tool", "thinking"),
        ("PreCompact", "pre_compact", "thinking"),
        ("PostCompact", "post_compact", "thinking"),
        ("SubagentStart", "subagent_start", "subagent"),
        ("SubagentStop", "subagent_stop", "thinking"),
        ("Stop", "stop", "success"),
        ("SessionEnd", "session_end", "idle"),
    ],
)
def test_native_hook_normalizes_every_lifecycle_event(
    native_hook: Path, tmp_path: Path, hook_name: str, event: str, state: str
) -> None:
    receiver = Receiver(tmp_path / "events.sock")
    try:
        result, datagram = receiver.invoke(
            native_hook,
            {
                "hook_event_name": hook_name,
                "session_id": "thr_private",
                "turn_id": "turn_private",
                "prompt": "must not cross the privacy boundary",
                "tool_input": {"command": "private command"},
                "tool_response": {"content": "private response"},
            },
        )
    finally:
        receiver.close()
    assert result.returncode == 0
    assert result.stderr == b""
    assert result.stdout == (b"{}" if hook_name in {"Stop", "SubagentStop"} else b"")
    assert set(datagram) == WIRE_FIELDS
    assert datagram["event"] == event
    assert datagram["state"] == state
    assert datagram["session"] == "thr_private"
    assert datagram["turn"] == "turn_private"
    serialized = json.dumps(datagram)
    assert "privacy boundary" not in serialized
    assert "private command" not in serialized
    assert "private response" not in serialized
    assert isinstance(datagram["time_ns"], int) and datagram["time_ns"] > 0


@pytest.mark.parametrize(
    ("tool", "tool_input", "expected"),
    [
        ("apply_patch", {}, "writing"),
        ("mcp__fs__write_file", {}, "writing"),
        ("Read", {}, "reading"),
        ("mcp__fs__search_query", {}, "reading"),
        ("spawn_agent", {}, "subagent"),
        ("Bash", {"command": "echo ok"}, "command"),
        ("Bash", {"command": "cd src && python -m pytest -q | tee result"}, "testing"),
        ("Bash", {"command": "npm run 'test'"}, "testing"),
        ("Bash", {"command": '"arduino-cli" upload -b arduino:zephyr:unoq'}, "flashing"),
        ("Bash", {"command": "sudo -u arduino env A=1 west flash"}, "flashing"),
        ("Bash", {"command": "cmake --build build && ninja"}, "building"),
        ("Bash", {"command": "bash -lc 'python -m pytest -q'"}, "testing"),
        ("Bash", {"command": ["cd src", "cargo test"]}, "testing"),
        ("Bash", {"command": []}, "command"),
        ("Bash", {"command": ["pytest", 7]}, "command"),
        ("Bash", {"command": None, "cmd": "pytest -q"}, "testing"),
        ("Bash", {"command": ["pytest", 7], "script": "ninja"}, "building"),
        ("Bash", {"command": "echo 'pytest && make'"}, "command"),
        ("Bash", {"command": "pytest 'unterminated"}, "command"),
        ("Bash", {"command": "make && pytest && arduino-cli upload"}, "flashing"),
    ],
)
def test_native_hook_classifies_tools(
    native_hook: Path, tmp_path: Path, tool: str, tool_input: object, expected: str
) -> None:
    receiver = Receiver(tmp_path / "events.sock")
    try:
        result, datagram = receiver.invoke(
            native_hook,
            {
                "hook_event_name": "PreToolUse",
                "session_id": "thr_test",
                "turn_id": "turn_test",
                "tool_use_id": "item_test",
                "tool_name": tool,
                "tool_input": tool_input,
            },
        )
    finally:
        receiver.close()
    assert result.returncode == 0
    assert result.stdout == result.stderr == b""
    assert datagram["state"] == expected
    assert datagram["tool"] == tool
    assert "command" not in datagram


@pytest.mark.parametrize(
    ("response", "expected_state", "failed"),
    [
        ({"exit_code": 2, "output": "private"}, "error", True),
        ({"exitCode": " 3 "}, "error", True),
        ({"status": " FAILED "}, "error", True),
        ({"success": False}, "error", True),
        ({"is_error": True}, "error", True),
        ({"error": {"code": 1}}, "error", True),
        ({"metadata": {"return_code": 4}}, "error", True),
        ({"error": {}}, "thinking", False),
        ({"error": []}, "thinking", False),
        ({"error": 0}, "thinking", False),
        ({"content": "the words error and failed are only text"}, "thinking", False),
    ],
)
def test_native_hook_uses_only_explicit_failure(
    native_hook: Path, tmp_path: Path, response: object, expected_state: str, failed: bool
) -> None:
    receiver = Receiver(tmp_path / "events.sock")
    try:
        _, datagram = receiver.invoke(
            native_hook,
            {
                "hook_event_name": "PostToolUse",
                "session_id": "thr_test",
                "tool_name": "Bash",
                "tool_response": response,
            },
        )
    finally:
        receiver.close()
    assert datagram["state"] == expected_state
    assert datagram["failed"] is failed


def test_native_hook_aliases_and_json_escapes(native_hook: Path, tmp_path: Path) -> None:
    receiver = Receiver(tmp_path / "events.sock")
    try:
        _, datagram = receiver.invoke(
            native_hook,
            b'{"hookEventName":"UserPromptSubmit","sessionId":"thr_\\u2603","turnId":"turn_1"}',
        )
    finally:
        receiver.close()
    assert datagram["session"] == "thr_\N{SNOWMAN}"
    assert datagram["event"] == "user_prompt"


def test_native_hook_is_fail_open_for_invalid_input(native_hook: Path, tmp_path: Path) -> None:
    path = str(tmp_path / "absent.sock")
    invalid = [
        b"not json",
        b"[]",
        b'{"hook_event_name":"Unknown","session_id":"thr"}',
        b'{"hook_event_name":"SessionStart"}',
        b'{"hook_event_name":"SessionStart","session_id":"bad\\u0000id"}',
        b'{"hook_event_name":"SessionStart","session_id":"thr",}',
        b'{' + b'x' * 70_000,
    ]
    for payload in invalid:
        result = _run(native_hook, payload, path)
        assert result.returncode == 0
        assert result.stdout == result.stderr == b""


def test_native_hook_replies_to_stop_without_daemon(native_hook: Path, tmp_path: Path) -> None:
    path = str(tmp_path / "absent.sock")
    for name in ("Stop", "SubagentStop"):
        payload = json.dumps({"hook_event_name": name, "session_id": "thr"}).encode()
        result = _run(native_hook, payload, path)
        assert result.returncode == 0
        assert result.stdout == b"{}"
        assert result.stderr == b""
    for name in ("Stop", "SubagentStop"):
        oversized = json.dumps(
            {"hook_event_name": name, "session_id": "thr", "padding": "x" * 70_000}
        ).encode()
        assert _run(native_hook, oversized, path).stdout == b"{}"


def test_native_hook_is_fail_open_when_stop_stdout_is_closed(
    native_hook: Path, tmp_path: Path
) -> None:
    environment = os.environ.copy()
    environment["UNOQ_CODEX_MATRIX_EVENT_SOCKET"] = str(tmp_path / "absent.sock")
    read_fd, write_fd = os.pipe()
    os.close(read_fd)
    try:
        result = subprocess.run(
            [str(native_hook)],
            input=b'{"hook_event_name":"Stop","session_id":"thr"}',
            stdout=write_fd,
            stderr=subprocess.PIPE,
            env=environment,
            timeout=1,
            check=False,
        )
    finally:
        os.close(write_fd)
    assert result.returncode == 0
    assert result.stderr == b""


def _p95(values: list[float]) -> float:
    return sorted(values)[math.ceil(len(values) * 0.95) - 1]


@pytest.mark.parametrize("daemon_running", [False, True])
def test_native_hook_p95_under_50_ms(
    native_hook: Path, tmp_path: Path, daemon_running: bool
) -> None:
    path = str(tmp_path / "events.sock")
    server = None
    worker = None
    running = True
    if daemon_running:
        server = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        server.bind(path)
        server.settimeout(0.05)

        def drain() -> None:
            assert server is not None
            while running:
                try:
                    server.recv(4096)
                except (TimeoutError, OSError):
                    pass

        worker = threading.Thread(target=drain, daemon=True)
        worker.start()
    timings = []
    try:
        for _ in range(120):
            started = time.perf_counter_ns()
            result = _run(native_hook, PAYLOAD, path)
            timings.append((time.perf_counter_ns() - started) / 1_000_000)
            assert result.returncode == 0
            assert result.stdout == result.stderr == b""
    finally:
        running = False
        if worker is not None:
            worker.join(timeout=1)
        if server is not None:
            server.close()
    assert _p95(timings[10:]) < 50
