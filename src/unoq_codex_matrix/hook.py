"""Short-lived, fail-open Codex lifecycle hook client."""

from __future__ import annotations

import json
import os
import socket
import sys
from collections.abc import Mapping


EVENT_SOCKET = "/run/unoq-codex-matrix/events.sock"
MAX_HOOK_INPUT_BYTES = 65_536


def _event_needs_json_stdout(payload: object, raw_prefix: bytes) -> bool:
    if isinstance(payload, Mapping):
        value = payload.get("hook_event_name")
        if value is None:
            value = payload.get("hookEventName")
        if isinstance(value, str):
            return value.lower().replace("_", "") in {"stop", "subagentstop"}
    # Oversized or malformed Stop input must still receive valid JSON.  The
    # event name occurs near the beginning of official payloads; no content is
    # retained and this branch only influences stdout.
    compact = raw_prefix[:2048].replace(b" ", b"").replace(b"\n", b"")
    return b'"hook_event_name":"Stop"' in compact or b'"hook_event_name":"SubagentStop"' in compact


def _socket_path() -> str:
    # The override exists for tests and private development only.  AF_UNIX is
    # always used, so the hook cannot be redirected to a network endpoint.
    path = os.environ.get("UNOQ_CODEX_MATRIX_EVENT_SOCKET", EVENT_SOCKET)
    if not path or "\x00" in path or len(path.encode(errors="ignore")) >= 104:
        return EVENT_SOCKET
    return path


def _send(datagram: bytes) -> None:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        sock.setblocking(False)
        sock.sendto(datagram, _socket_path())
    except (BlockingIOError, OSError, ValueError):
        # A missing, busy, or restarting daemon must never affect Codex.
        pass
    finally:
        try:
            sock.close()
        except OSError:
            pass


def main() -> int:
    payload: object = None
    raw = b""
    try:
        raw = sys.stdin.buffer.read(MAX_HOOK_INPUT_BYTES + 1)
        if len(raw) <= MAX_HOOK_INPUT_BYTES:
            payload = json.loads(raw.decode("utf-8"))
            if isinstance(payload, Mapping):
                # Import the classifier only after bounded JSON input succeeds.
                from .events import encode_event, normalize_hook_payload

                _send(encode_event(normalize_hook_payload(payload)))
    except Exception:
        # Includes malformed JSON, invalid fields, oversized normalized data,
        # and all local socket errors.  This executable is intentionally
        # fail-open and never logs raw input.
        pass
    try:
        if _event_needs_json_stdout(payload, raw):
            os.write(sys.stdout.fileno(), b"{}")
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
