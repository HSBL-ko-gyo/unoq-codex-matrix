"""Arduino Router MessagePack-RPC client.

The client uses only the documented Router Unix stream socket.  It never opens
the MCU serial device directly.
"""

from __future__ import annotations

from dataclasses import dataclass
import itertools
import socket
import threading
import time
from typing import Any

import msgpack

from .protocol import PROTOCOL_VERSION, State


ROUTER_SOCKET = "/var/run/arduino-router.sock"
MAX_RPC_BUFFER = 4096


class RouterError(RuntimeError):
    """The Arduino Router or MCU did not complete an RPC request."""


@dataclass(frozen=True)
class McuStatus:
    protocol_version: int
    state: int
    active_sessions: int
    brightness: int


@dataclass(frozen=True)
class FirmwareVersion:
    protocol_version: int
    major: int
    minor: int
    patch: int

    def __str__(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"

    @property
    def supports_quota_bar(self) -> bool:
        return (self.major, self.minor, self.patch) >= (0, 2, 0)


@dataclass(frozen=True)
class RenderMetrics:
    average_us: int
    max_us: int


class RouterBridge:
    """Small synchronous RPC client with reconnect-on-next-call semantics."""

    def __init__(
        self,
        path: str = ROUTER_SOCKET,
        *,
        connect_timeout: float = 0.25,
        response_timeout: float = 0.5,
    ) -> None:
        self.path = path
        self.connect_timeout = connect_timeout
        self.response_timeout = response_timeout
        self._socket: socket.socket | None = None
        self._unpacker: msgpack.Unpacker | None = None
        self._ids = itertools.count(1)
        self._lock = threading.Lock()
        self.last_success_monotonic: float | None = None
        self.last_error: str | None = None

    @property
    def connected(self) -> bool:
        return self._socket is not None

    def close(self) -> None:
        sock, self._socket = self._socket, None
        self._unpacker = None
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass

    def _connect(self) -> None:
        if self._socket is not None:
            return
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.settimeout(self.connect_timeout)
            sock.connect(self.path)
            sock.settimeout(self.response_timeout)
        except OSError:
            sock.close()
            raise
        self._socket = sock
        self._unpacker = msgpack.Unpacker(
            raw=False,
            strict_map_key=False,
            max_buffer_size=MAX_RPC_BUFFER,
        )

    def request(self, method: str, params: list[Any] | None = None) -> Any:
        """Send one MessagePack-RPC request and return its validated result."""

        if not isinstance(method, str) or not method or len(method) > 96:
            raise ValueError("invalid RPC method")
        params = [] if params is None else params
        with self._lock:
            message_id = next(self._ids) & 0xFFFFFFFF
            if message_id == 0:
                message_id = next(self._ids) & 0xFFFFFFFF
            try:
                self._connect()
                assert self._socket is not None
                assert self._unpacker is not None
                request = [0, message_id, method, params]
                encoded = msgpack.packb(request, use_bin_type=True)
                if len(encoded) > MAX_RPC_BUFFER:
                    raise ValueError("RPC request exceeds buffer limit")
                self._socket.sendall(encoded)
                deadline = time.monotonic() + self.response_timeout
                while time.monotonic() < deadline:
                    remaining = max(0.01, deadline - time.monotonic())
                    self._socket.settimeout(remaining)
                    chunk = self._socket.recv(MAX_RPC_BUFFER)
                    if not chunk:
                        raise ConnectionError("Arduino Router disconnected")
                    self._unpacker.feed(chunk)
                    for response in self._unpacker:
                        if not isinstance(response, list) or len(response) != 4:
                            continue
                        if response[0] != 1 or response[1] != message_id:
                            continue
                        error, result = response[2], response[3]
                        if error not in (None, False, ""):
                            raise RouterError("MCU returned an RPC error")
                        self.last_success_monotonic = time.monotonic()
                        self.last_error = None
                        return result
                raise TimeoutError("Arduino Router RPC timed out")
            except (OSError, ValueError, TypeError, msgpack.UnpackException) as exc:
                self.last_error = type(exc).__name__
                self.close()
                raise RouterError(f"Arduino Router RPC failed: {type(exc).__name__}") from exc
            except RouterError:
                self.last_error = "RouterError"
                self.close()
                raise
            except TimeoutError as exc:
                self.last_error = "TimeoutError"
                self.close()
                raise RouterError("Arduino Router RPC timed out") from exc

    @staticmethod
    def _bounded_int(value: Any, low: int, high: int, field: str) -> int:
        if isinstance(value, bool) or not isinstance(value, int):
            raise RouterError(f"invalid {field} response")
        if not low <= value <= high:
            raise RouterError(f"out-of-range {field} response")
        return value

    def set_state(
        self,
        state: State | int,
        active_sessions: int,
        *,
        frame_interval_ms: int,
        offline_timeout_s: float,
        show_active_count: bool,
    ) -> bool:
        result = self.request(
            "codex_matrix_set_state",
            [
                PROTOCOL_VERSION,
                int(state),
                min(max(int(active_sessions), 0), 255),
                int(frame_interval_ms),
                int(offline_timeout_s * 1000),
                int(bool(show_active_count)),
            ],
        )
        return bool(self._bounded_int(result, 0, 1, "set-state"))

    def heartbeat(self) -> bool:
        result = self.request("codex_matrix_heartbeat", [PROTOCOL_VERSION])
        return bool(self._bounded_int(result, 0, 1, "heartbeat"))

    def set_brightness(self, brightness: int) -> int:
        result = self.request(
            "codex_matrix_set_brightness", [PROTOCOL_VERSION, int(brightness)]
        )
        return self._bounded_int(result, 0, 7, "brightness")

    def set_quota(self, remaining_percent: int, *, visible: bool) -> bool:
        result = self.request(
            "codex_matrix_set_quota",
            [PROTOCOL_VERSION, int(remaining_percent), int(bool(visible))],
        )
        return bool(self._bounded_int(result, 0, 1, "set-quota"))

    def get_status(self) -> McuStatus:
        packed = self._bounded_int(
            self.request("codex_matrix_get_status"), 0, 0xFFFFFFFF, "status"
        )
        return McuStatus(
            protocol_version=(packed >> 24) & 0xFF,
            state=(packed >> 16) & 0xFF,
            active_sessions=(packed >> 8) & 0xFF,
            brightness=packed & 0xFF,
        )

    def get_version(self) -> FirmwareVersion:
        packed = self._bounded_int(
            self.request("codex_matrix_get_version"), 0, 0xFFFFFFFF, "version"
        )
        return FirmwareVersion(
            protocol_version=(packed >> 24) & 0xFF,
            major=(packed >> 16) & 0xFF,
            minor=(packed >> 8) & 0xFF,
            patch=packed & 0xFF,
        )

    def get_render_metrics(self) -> RenderMetrics:
        packed = self._bounded_int(
            self.request("codex_matrix_get_render_metrics"),
            0,
            0xFFFFFFFF,
            "render-metrics",
        )
        return RenderMetrics(
            average_us=(packed >> 16) & 0xFFFF,
            max_us=packed & 0xFFFF,
        )

    def publish(
        self,
        state: State | int,
        active_sessions: int,
        *,
        brightness: int,
        frame_interval_ms: int,
        offline_timeout_s: float,
        show_active_count: bool,
        quota_remaining_percent: int | None = None,
        show_quota_bar: bool = False,
    ) -> tuple[McuStatus, FirmwareVersion]:
        """Publish complete state, then heartbeat, so MCU restarts self-heal."""

        version = self.get_version()
        if version.protocol_version != PROTOCOL_VERSION:
            raise RouterError("firmware protocol version mismatch")
        if not self.set_state(
            state,
            active_sessions,
            frame_interval_ms=frame_interval_ms,
            offline_timeout_s=offline_timeout_s,
            show_active_count=show_active_count,
        ):
            raise RouterError("MCU rejected state")
        self.set_brightness(brightness)
        if version.supports_quota_bar:
            remaining = (
                0 if quota_remaining_percent is None else quota_remaining_percent
            )
            if not 0 <= remaining <= 100:
                raise ValueError("quota remaining percentage is out of range")
            if not self.set_quota(
                remaining,
                visible=show_quota_bar and quota_remaining_percent is not None,
            ):
                raise RouterError("MCU rejected quota")
        if not self.heartbeat():
            raise RouterError("MCU rejected heartbeat")
        status = self.get_status()
        if status.protocol_version != PROTOCOL_VERSION:
            raise RouterError("MCU protocol version mismatch")
        return status, version
