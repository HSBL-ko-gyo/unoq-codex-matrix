"""Command-line control client."""

from __future__ import annotations

import argparse
import sys
from typing import Any, Sequence

from . import __version__
from .control import CONTROL_SOCKET, ControlError, request
from .protocol import PROTOCOL_VERSION, State


STATE_CHOICES = tuple(state.name.lower() for state in State)


def _age(value: Any) -> str:
    if value is None:
        return "never"
    try:
        seconds = max(0.0, float(value))
    except (TypeError, ValueError):
        return "unknown"
    return f"{seconds:.1f}s"


def _print_status(response: dict[str, Any]) -> None:
    fields = (
        ("daemon status", response.get("daemon_status", "unknown")),
        ("Arduino Router status", response.get("router_status", "unknown")),
        ("MCU protocol version", response.get("mcu_protocol_version", "unknown")),
        ("current global state", response.get("current_state", "unknown")),
        ("active session count", response.get("active_session_count", 0)),
        ("last hook event age", _age(response.get("last_hook_event_age_s"))),
        ("last MCU heartbeat age", _age(response.get("last_mcu_heartbeat_age_s"))),
        ("brightness", response.get("brightness", "unknown")),
        ("firmware version", response.get("firmware_version", "unknown")),
    )
    width = max(len(name) for name, _ in fields)
    for name, value in fields:
        print(f"{name:<{width}} : {value}")


def _call(message: dict[str, Any], socket_path: str) -> dict[str, Any]:
    response = request(message, path=socket_path)
    if response.get("ok") is not True:
        raise ControlError(str(response.get("error", "daemon rejected request")))
    return response


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="unoq-codex-matrix")
    parser.add_argument("--socket", default=CONTROL_SOCKET, help=argparse.SUPPRESS)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("status", help="show daemon, Router, and MCU status")
    subparsers.add_parser("demo", help="show every state for about two seconds")
    set_parser = subparsers.add_parser("set", help="temporarily override the display")
    set_parser.add_argument("state", choices=STATE_CHOICES)
    set_parser.add_argument("--seconds", type=float, default=10.0)
    subparsers.add_parser("off", help="temporarily turn the matrix off")
    subparsers.add_parser("doctor", help="run local health checks")
    subparsers.add_parser("version", help="show client and protocol versions")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.command == "version":
        print(f"unoq-codex-matrix {__version__} (protocol {PROTOCOL_VERSION})")
        return 0
    try:
        if args.command == "status":
            _print_status(_call({"action": "status"}, args.socket))
            return 0
        if args.command == "demo":
            response = _call({"action": "demo"}, args.socket)
            print(
                "Demo started; the daemon will return to IDLE after "
                f"{response.get('duration_s', 0):g}s."
            )
            return 0
        if args.command == "set":
            if not 0.1 <= args.seconds <= 3600:
                raise ControlError("--seconds must be between 0.1 and 3600")
            _call(
                {
                    "action": "set",
                    "state": args.state,
                    "duration_s": args.seconds,
                },
                args.socket,
            )
            print(f"Display override: {args.state.upper()} for {args.seconds:g}s")
            return 0
        if args.command == "off":
            _call(
                {"action": "set", "state": "off", "duration_s": 30.0},
                args.socket,
            )
            print("Display override: OFF for 30s")
            return 0
        if args.command == "doctor":
            response = _call({"action": "doctor"}, args.socket)
            _print_status(response)
            checks = response.get("checks", {})
            if isinstance(checks, dict):
                for name, passed in checks.items():
                    print(f"{name}: {'ok' if passed else 'FAILED'}")
            return 0 if response.get("healthy") else 1
    except ControlError as exc:
        print(f"unoq-codex-matrix: {exc}", file=sys.stderr)
        return 1
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
