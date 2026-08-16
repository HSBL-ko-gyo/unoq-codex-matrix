from __future__ import annotations

from pathlib import Path
import socket
import threading

import msgpack
import pytest

from unoq_codex_matrix.bridge import RouterBridge
from unoq_codex_matrix.protocol import State


@pytest.mark.skipif(not hasattr(socket, "AF_UNIX"), reason="requires Unix sockets")
def test_publish_uses_documented_msgpack_rpc(tmp_path: Path) -> None:
    path = str(tmp_path / "router.sock")
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(path)
    server.listen(1)
    methods: list[tuple[str, list[object]]] = []

    def serve() -> None:
        connection, _ = server.accept()
        unpacker = msgpack.Unpacker(raw=False)
        with connection:
            while len(methods) < 5:
                chunk = connection.recv(4096)
                if not chunk:
                    return
                unpacker.feed(chunk)
                for message in unpacker:
                    message_type, message_id, method, params = message
                    assert message_type == 0
                    methods.append((method, params))
                    if method in {"codex_matrix_set_state", "codex_matrix_heartbeat"}:
                        result = 1
                    elif method == "codex_matrix_set_brightness":
                        result = 3
                    elif method == "codex_matrix_get_status":
                        result = (1 << 24) | (7 << 16) | (2 << 8) | 3
                    else:
                        result = (1 << 24) | (0 << 16) | (1 << 8) | 0
                    connection.sendall(msgpack.packb([1, message_id, None, result]))

    thread = threading.Thread(target=serve)
    thread.start()
    try:
        bridge = RouterBridge(path, connect_timeout=0.5, response_timeout=0.5)
        status, version = bridge.publish(
            State.TESTING,
            2,
            brightness=3,
            frame_interval_ms=100,
            offline_timeout_s=12,
            show_active_count=True,
        )
        bridge.close()
    finally:
        thread.join(timeout=2)
        server.close()
    assert [method for method, _ in methods] == [
        "codex_matrix_set_state",
        "codex_matrix_set_brightness",
        "codex_matrix_heartbeat",
        "codex_matrix_get_status",
        "codex_matrix_get_version",
    ]
    assert methods[0][1] == [1, 7, 2, 100, 12_000, 1]
    assert status.state == State.TESTING
    assert str(version) == "0.1.0"
