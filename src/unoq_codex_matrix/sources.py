"""Extensible event-source boundary for the status daemon.

Version 0.1 intentionally enables only :class:`CodexHooksSource`.  Future
App Server or experimental HID inputs can implement :class:`EventSource`
without coupling their transport to session aggregation or MCU rendering.
"""

from __future__ import annotations

from collections.abc import Iterator
import os
import socket
import stat
from typing import Protocol, runtime_checkable

from .protocol import MAX_EVENT_BYTES


@runtime_checkable
class EventSource(Protocol):
    """A bounded, local producer of normalized event datagrams."""

    @property
    def fileobj(self) -> socket.socket:
        """Return the nonblocking descriptor registered with the selector."""

    def open(self) -> None:
        """Acquire the source transport."""

    def receive(self, limit: int = 128) -> Iterator[bytes]:
        """Yield at most *limit* currently available messages."""

    def close(self) -> None:
        """Release source resources and its owned filesystem endpoint."""


class CodexHooksSource:
    """Receive privacy-normalized lifecycle events over Unix datagrams."""

    def __init__(self, path: str) -> None:
        self.path = path
        self._socket: socket.socket | None = None

    @property
    def fileobj(self) -> socket.socket:
        if self._socket is None:
            raise RuntimeError("event source is not open")
        return self._socket

    def open(self) -> None:
        if self._socket is not None:
            return
        parent = os.path.dirname(self.path)
        os.makedirs(parent, mode=0o750, exist_ok=True)
        try:
            mode = os.stat(self.path, follow_symlinks=False).st_mode
        except FileNotFoundError:
            pass
        else:
            if not stat.S_ISSOCK(mode):
                raise RuntimeError(f"refusing to replace non-socket path: {self.path}")
            os.unlink(self.path)

        source_socket = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            source_socket.setblocking(False)
            source_socket.bind(self.path)
            os.chmod(self.path, 0o660)
        except Exception:
            source_socket.close()
            raise
        self._socket = source_socket

    def receive(self, limit: int = 128) -> Iterator[bytes]:
        source_socket = self.fileobj
        for _ in range(max(0, min(limit, 1024))):
            try:
                yield source_socket.recv(MAX_EVENT_BYTES + 1)
            except BlockingIOError:
                return

    def close(self) -> None:
        source_socket, self._socket = self._socket, None
        if source_socket is not None:
            try:
                source_socket.close()
            except OSError:
                pass
        try:
            os.unlink(self.path)
        except FileNotFoundError:
            pass
