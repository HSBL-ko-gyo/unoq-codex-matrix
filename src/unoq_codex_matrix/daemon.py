"""UNO Q Codex Matrix event daemon."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, fields
import json
import logging
import os
from pathlib import Path
import selectors
import signal
import socket
import time
from typing import Any, Callable, Sequence

from .aggregate import AggregateSnapshot, SessionAggregator
from .bridge import FirmwareVersion, McuStatus, ROUTER_SOCKET, RouterBridge, RouterError
from .control import MAX_CONTROL_MESSAGE, decode_message, encode_message
from .events import EventError, decode_event
from .protocol import MAX_EVENT_BYTES, PROTOCOL_VERSION, State
from .quota import CodexQuotaSource
from .sources import CodexHooksSource, EventSource


EVENT_SOCKET = "/run/unoq-codex-matrix/events.sock"
CONTROL_SOCKET = "/run/unoq-codex-matrix/control.sock"
DEFAULT_CONFIG = "/etc/unoq-codex-matrix/config.json"
LOG = logging.getLogger("unoq-codex-matrixd")


@dataclass(frozen=True, slots=True)
class Config:
    brightness: int = 3
    frame_interval_ms: int = 100
    heartbeat_interval_s: float = 3.0
    offline_timeout_s: float = 12.0
    success_hold_s: float = 8.0
    transient_error_s: float = 1.5
    stale_session_s: float = 43_200.0
    show_active_count: bool = True
    show_quota_bar: bool = True
    quota_refresh_interval_s: float = 60.0
    quota_stale_after_s: float = 900.0
    log_level: str = "INFO"


_RANGES: dict[str, tuple[float, float]] = {
    "brightness": (0, 5),
    "frame_interval_ms": (50, 150),
    "heartbeat_interval_s": (2, 5),
    "offline_timeout_s": (6, 120),
    "success_hold_s": (1, 60),
    "transient_error_s": (0.2, 10),
    "stale_session_s": (60, 604_800),
    "quota_refresh_interval_s": (30, 3_600),
    "quota_stale_after_s": (60, 86_400),
}


def load_config(path: str = DEFAULT_CONFIG) -> Config:
    defaults = Config()
    try:
        with open(path, "rb") as handle:
            raw = json.load(handle)
    except FileNotFoundError:
        LOG.warning("configuration file is absent; using safe defaults")
        return defaults
    except (OSError, json.JSONDecodeError, UnicodeDecodeError):
        LOG.warning("configuration is unreadable; using safe defaults")
        return defaults
    if not isinstance(raw, dict):
        LOG.warning("configuration root is invalid; using safe defaults")
        return defaults

    values = {field.name: getattr(defaults, field.name) for field in fields(Config)}
    for name, (minimum, maximum) in _RANGES.items():
        value = raw.get(name, values[name])
        expected_int = name in {"brightness", "frame_interval_ms"}
        valid_number = isinstance(value, (int, float)) and not isinstance(value, bool)
        if not valid_number or not minimum <= float(value) <= maximum:
            LOG.warning("invalid configuration field %s; using default", name)
            continue
        if expected_int and not isinstance(value, int):
            LOG.warning("configuration field %s must be an integer; using default", name)
            continue
        values[name] = value
    for name in ("show_active_count", "show_quota_bar"):
        enabled = raw.get(name, values[name])
        if isinstance(enabled, bool):
            values[name] = enabled
        else:
            LOG.warning("invalid configuration field %s; using default", name)
    log_level = raw.get("log_level", values["log_level"])
    if isinstance(log_level, str) and log_level.upper() in {
        "DEBUG",
        "INFO",
        "WARNING",
        "ERROR",
    }:
        values["log_level"] = log_level.upper()
    else:
        LOG.warning("invalid configuration field log_level; using default")
    if values["offline_timeout_s"] < values["heartbeat_interval_s"] * 2:
        LOG.warning("offline timeout is too short; using default")
        values["offline_timeout_s"] = defaults.offline_timeout_s
    if values["quota_stale_after_s"] < values["quota_refresh_interval_s"] * 2:
        LOG.warning("quota stale timeout is too short; using default")
        values["quota_stale_after_s"] = defaults.quota_stale_after_s
    return Config(**values)


DEMO_STATES: tuple[State, ...] = tuple(State) + (State.IDLE,)


class MatrixDaemon:
    def __init__(
        self,
        config: Config,
        *,
        event_path: str = EVENT_SOCKET,
        control_path: str = CONTROL_SOCKET,
        bridge: RouterBridge | None = None,
        event_source: EventSource | None = None,
        quota_source: CodexQuotaSource | None = None,
        monotonic: Callable[[], float] = time.monotonic,
        monotonic_ns: Callable[[], int] = time.monotonic_ns,
    ) -> None:
        self.config = config
        self.event_path = event_path
        self.control_path = control_path
        self.bridge = bridge or RouterBridge()
        self.event_source = event_source or CodexHooksSource(event_path)
        self.monotonic = monotonic
        self.monotonic_ns = monotonic_ns
        self.quota_source = quota_source or CodexQuotaSource(
            refresh_interval_s=config.quota_refresh_interval_s,
            stale_after_s=config.quota_stale_after_s,
            monotonic=monotonic,
        )
        self.aggregator = SessionAggregator(
            success_hold_s=config.success_hold_s,
            transient_error_s=config.transient_error_s,
            stale_session_s=config.stale_session_s,
        )
        self.selector = selectors.DefaultSelector()
        self.event_socket: socket.socket | None = None
        self.control_socket: socket.socket | None = None
        self.clients: dict[socket.socket, bytearray] = {}
        self.running = False
        self.last_hook_monotonic: float | None = None
        self.last_publish_attempt = 0.0
        self.last_display: tuple[State, int, int | None] | None = None
        self.last_mcu_status: McuStatus | None = None
        self.last_firmware_version: FirmwareVersion | None = None
        self.last_publish_ok = False
        self.override_state: State | None = None
        self.override_expiry = 0.0
        self.demo_started: float | None = None
        self.demo_step_s = 2.0
        self.invalid_events = 0
        self._last_invalid_log = 0.0
        self._last_router_log = 0.0
        self._next_router_attempt = 0.0

    @staticmethod
    def _prepare_parent(path: str) -> None:
        parent = os.path.dirname(path)
        os.makedirs(parent, mode=0o750, exist_ok=True)

    @staticmethod
    def _remove_socket(path: str) -> None:
        try:
            if os.path.lexists(path):
                mode = os.stat(path, follow_symlinks=False).st_mode
                if not __import__("stat").S_ISSOCK(mode):
                    raise RuntimeError(f"refusing to replace non-socket path: {path}")
                os.unlink(path)
        except FileNotFoundError:
            pass

    def setup(self) -> None:
        self.event_source.open()
        self.event_socket = self.event_source.fileobj
        self.selector.register(self.event_socket, selectors.EVENT_READ, "events")

        self._prepare_parent(self.control_path)
        self._remove_socket(self.control_path)
        control_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        control_sock.setblocking(False)
        control_sock.bind(self.control_path)
        os.chmod(self.control_path, 0o660)
        control_sock.listen(16)
        self.control_socket = control_sock
        self.selector.register(control_sock, selectors.EVENT_READ, "listener")
        if self.config.show_quota_bar:
            self.quota_source.start()

    def close(self) -> None:
        for client in list(self.clients):
            self._close_client(client)
        if self.event_socket is not None:
            try:
                self.selector.unregister(self.event_socket)
            except Exception:
                pass
        self.event_source.close()
        self.quota_source.close()
        self.event_socket = None
        for sock in (self.control_socket,):
            if sock is not None:
                try:
                    self.selector.unregister(sock)
                except Exception:
                    pass
                try:
                    sock.close()
                except OSError:
                    pass
        self.control_socket = None
        self.bridge.close()
        self.selector.close()
        for path in (self.control_path,):
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass

    def stop(self, *_: object) -> None:
        self.running = False

    def _invalid(self) -> None:
        self.invalid_events += 1
        now = self.monotonic()
        if now - self._last_invalid_log >= 60:
            LOG.warning("discarded invalid hook datagram(s); count=%d", self.invalid_events)
            self._last_invalid_log = now

    def _receive_events(self) -> None:
        try:
            messages = self.event_source.receive(128)
            for data in messages:
                if len(data) > MAX_EVENT_BYTES:
                    self._invalid()
                    continue
                try:
                    event = decode_event(data)
                    now_ns = self.monotonic_ns()
                    tolerance_ns = 5_000_000_000
                    stale_ns = int(self.config.stale_session_s * 1_000_000_000)
                    if event.time_ns > now_ns + tolerance_ns or event.time_ns < now_ns - stale_ns:
                        raise EventError("event timestamp is outside the accepted window")
                    self.aggregator.process(event, now_ns=now_ns)
                    self.last_hook_monotonic = self.monotonic()
                except (EventError, ValueError, TypeError):
                    self._invalid()
        except OSError:
            self._invalid()

    def _accept_client(self) -> None:
        assert self.control_socket is not None
        try:
            client, _ = self.control_socket.accept()
        except BlockingIOError:
            return
        client.setblocking(False)
        self.clients[client] = bytearray()
        self.selector.register(client, selectors.EVENT_READ, "client")

    def _close_client(self, client: socket.socket) -> None:
        self.clients.pop(client, None)
        try:
            self.selector.unregister(client)
        except Exception:
            pass
        try:
            client.close()
        except OSError:
            pass

    def _send_response(self, client: socket.socket, response: dict[str, Any]) -> None:
        try:
            client.setblocking(True)
            client.settimeout(0.2)
            client.sendall(encode_message(response))
        except (OSError, ValueError):
            pass
        finally:
            self._close_client(client)

    def _read_client(self, client: socket.socket) -> None:
        buffer = self.clients.get(client)
        if buffer is None:
            return
        try:
            chunk = client.recv(4096)
        except BlockingIOError:
            return
        except OSError:
            self._close_client(client)
            return
        if not chunk:
            self._close_client(client)
            return
        buffer.extend(chunk)
        if len(buffer) > MAX_CONTROL_MESSAGE:
            self._send_response(client, {"ok": False, "error": "request too large"})
            return
        newline = buffer.find(b"\n")
        if newline < 0:
            return
        try:
            message = decode_message(bytes(buffer[:newline]))
            response = self._handle_control(message)
        except Exception:
            response = {"ok": False, "error": "malformed request"}
        self._send_response(client, response)

    def _aggregate(self) -> AggregateSnapshot:
        return self.aggregator.snapshot(now_ns=self.monotonic_ns())

    def _display_state(self, now: float | None = None) -> tuple[State, int]:
        now = self.monotonic() if now is None else now
        if self.demo_started is not None:
            index = int((now - self.demo_started) // self.demo_step_s)
            if 0 <= index < len(DEMO_STATES):
                return DEMO_STATES[index], 0
            self.demo_started = None
            self.override_state = State.IDLE
            self.override_expiry = now + self.demo_step_s
        if self.override_state is not None:
            if now < self.override_expiry:
                return self.override_state, 0
            self.override_state = None
        snapshot = self._aggregate()
        return snapshot.state, snapshot.active_sessions

    def _publish(self, *, force: bool = False) -> bool:
        now = self.monotonic()
        state, active_sessions = self._display_state(now)
        quota = self.quota_source.current(now=now) if self.config.show_quota_bar else None
        quota_remaining = quota.remaining_percent if quota is not None else None
        display = (state, active_sessions, quota_remaining)
        heartbeat_due = now - self.last_publish_attempt >= self.config.heartbeat_interval_s
        if not force and display == self.last_display and not heartbeat_due:
            return True
        if not force and now < self._next_router_attempt:
            return False
        self.last_publish_attempt = now
        try:
            status, version = self.bridge.publish(
                display[0],
                display[1],
                brightness=self.config.brightness,
                frame_interval_ms=self.config.frame_interval_ms,
                offline_timeout_s=self.config.offline_timeout_s,
                show_active_count=self.config.show_active_count,
                quota_remaining_percent=quota_remaining,
                show_quota_bar=quota_remaining is not None,
            )
            self.last_mcu_status = status
            self.last_firmware_version = version
            self.last_display = display
            self.last_publish_ok = True
            self._next_router_attempt = 0.0
            return True
        except (RouterError, OSError, ValueError, TypeError) as exc:
            self.last_publish_ok = False
            self._next_router_attempt = now + 2.0
            if now - self._last_router_log >= 30:
                LOG.warning("Arduino Router/MCU unavailable: %s", type(exc).__name__)
                self._last_router_log = now
            return False

    def _age(self, timestamp: float | None) -> float | None:
        return None if timestamp is None else max(0.0, self.monotonic() - timestamp)

    def _status(self) -> dict[str, Any]:
        state, _ = self._display_state()
        # CLI overrides and demos intentionally suppress the active-count dots
        # sent to the MCU, but status must still report the real aggregate.
        count = self._aggregate().active_sessions
        mcu_age = self._age(self.bridge.last_success_monotonic)
        router_healthy = (
            self.last_publish_ok
            and mcu_age is not None
            and mcu_age <= self.config.heartbeat_interval_s * 2
        )
        quota = self.quota_source.current() if self.config.show_quota_bar else None
        response: dict[str, Any] = {
            "ok": True,
            "daemon_status": "running",
            "router_status": "connected" if router_healthy else "reconnecting",
            "mcu_protocol_version": (
                self.last_mcu_status.protocol_version if self.last_mcu_status else None
            ),
            "current_state": state.name,
            "active_session_count": count,
            "codex_quota_remaining_percent": (
                quota.remaining_percent if quota is not None else None
            ),
            "codex_quota_source_status": (
                self.quota_source.status()
                if self.config.show_quota_bar
                else "disabled"
            ),
            "last_hook_event_age_s": self._age(self.last_hook_monotonic),
            "last_mcu_heartbeat_age_s": mcu_age,
            "brightness": self.config.brightness,
            "firmware_version": (
                str(self.last_firmware_version) if self.last_firmware_version else None
            ),
            "firmware_quota_bar_supported": (
                self.last_firmware_version.supports_quota_bar
                if self.last_firmware_version
                else None
            ),
        }
        return response

    def _doctor(self) -> dict[str, Any]:
        # Doctor is a live probe, not a report of a cached response from an
        # earlier healthy MCU. Clear cached identity before the forced cycle.
        self.last_mcu_status = None
        self.last_firmware_version = None
        live_rpc = self._publish(force=True)
        response = self._status()
        checks = {
            "event socket": os.path.exists(self.event_path),
            "control socket": os.path.exists(self.control_path),
            "Arduino Router socket": os.path.exists(getattr(self.bridge, "path", ROUTER_SOCKET)),
            "MCU RPC": live_rpc and self.last_mcu_status is not None,
            "protocol match": bool(
                self.last_mcu_status
                and self.last_mcu_status.protocol_version == PROTOCOL_VERSION
                and self.last_firmware_version
                and self.last_firmware_version.protocol_version == PROTOCOL_VERSION
            ),
        }
        healthy = all(checks.values())
        if self.config.show_quota_bar:
            checks["Codex quota source"] = self.quota_source.current() is not None
        response["checks"] = checks
        # Quota is an optional overlay. Its outage must not turn a healthy
        # lifecycle/Router/MCU path into a failed installation.
        response["healthy"] = healthy
        return response

    def _handle_control(self, message: dict[str, Any]) -> dict[str, Any]:
        action = message.get("action")
        if action == "status":
            return self._status()
        if action == "doctor":
            return self._doctor()
        if action == "demo":
            self.demo_started = self.monotonic()
            self.override_state = None
            self.last_display = None
            self._publish(force=True)
            return {"ok": True, "duration_s": len(DEMO_STATES) * self.demo_step_s}
        if action == "set":
            try:
                state = State.parse(message.get("state"))
                duration = float(message.get("duration_s", 10))
            except (TypeError, ValueError):
                return {"ok": False, "error": "invalid state override"}
            if not 0.1 <= duration <= 3600:
                return {"ok": False, "error": "invalid override duration"}
            self.demo_started = None
            self.override_state = state
            self.override_expiry = self.monotonic() + duration
            self.last_display = None
            self._publish(force=True)
            return {"ok": True}
        return {"ok": False, "error": "unknown action"}

    def run(self) -> int:
        self.setup()
        self.running = True
        self._publish(force=True)
        try:
            while self.running:
                for key, _ in self.selector.select(timeout=0.1):
                    if key.data == "events":
                        self._receive_events()
                    elif key.data == "listener":
                        self._accept_client()
                    elif key.data == "client":
                        self._read_client(key.fileobj)
                self.aggregator.tick(now_ns=self.monotonic_ns())
                self._publish()
        finally:
            self.close()
        return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="unoq-codex-matrixd")
    parser.add_argument("--config", default=DEFAULT_CONFIG)
    parser.add_argument("--event-socket", default=EVENT_SOCKET, help=argparse.SUPPRESS)
    parser.add_argument("--control-socket", default=CONTROL_SOCKET, help=argparse.SUPPRESS)
    parser.add_argument("--router-socket", default=ROUTER_SOCKET, help=argparse.SUPPRESS)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    config = load_config(args.config)
    logging.basicConfig(
        level=getattr(logging, config.log_level),
        format="%(levelname)s %(name)s: %(message)s",
    )
    daemon = MatrixDaemon(
        config,
        event_path=args.event_socket,
        control_path=args.control_socket,
        bridge=RouterBridge(path=args.router_socket),
    )
    signal.signal(signal.SIGTERM, daemon.stop)
    signal.signal(signal.SIGINT, daemon.stop)
    LOG.info("started protocol=%d", PROTOCOL_VERSION)
    return daemon.run()


if __name__ == "__main__":
    raise SystemExit(main())
