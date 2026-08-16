"""Local control-socket protocol shared by the daemon and CLI."""

from __future__ import annotations

import json
import socket
from typing import Any


CONTROL_SOCKET = "/run/unoq-codex-matrix/control.sock"
MAX_CONTROL_MESSAGE = 8192


class ControlError(RuntimeError):
    pass


def encode_message(message: dict[str, Any]) -> bytes:
    encoded = json.dumps(
        message, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("ascii")
    if len(encoded) > MAX_CONTROL_MESSAGE:
        raise ControlError("control message is too large")
    return encoded + b"\n"


def decode_message(data: bytes) -> dict[str, Any]:
    if not data or len(data) > MAX_CONTROL_MESSAGE:
        raise ControlError("invalid control message size")
    try:
        value = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ControlError("malformed control message") from exc
    if not isinstance(value, dict):
        raise ControlError("control message must be an object")
    return value


def request(
    message: dict[str, Any],
    *,
    path: str = CONTROL_SOCKET,
    timeout: float = 2.0,
) -> dict[str, Any]:
    """Perform one bounded request/response exchange over the local socket."""

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.settimeout(timeout)
        sock.connect(path)
        sock.sendall(encode_message(message))
        chunks = bytearray()
        while len(chunks) <= MAX_CONTROL_MESSAGE:
            chunk = sock.recv(min(4096, MAX_CONTROL_MESSAGE + 1 - len(chunks)))
            if not chunk:
                break
            chunks.extend(chunk)
            newline = chunks.find(b"\n")
            if newline >= 0:
                return decode_message(bytes(chunks[:newline]))
        raise ControlError("daemon returned no complete response")
    except (OSError, TimeoutError) as exc:
        raise ControlError(f"cannot contact daemon: {type(exc).__name__}") from exc
    finally:
        sock.close()
