"""Privacy-bounded Codex quota reader backed by the official app server."""

from __future__ import annotations

from dataclasses import dataclass
import json
import queue
import subprocess
import threading
import time
from typing import Any, Callable, IO, Sequence


class QuotaError(RuntimeError):
    """Codex quota data was unavailable or malformed."""


@dataclass(frozen=True, slots=True)
class QuotaSnapshot:
    """Only the percentages needed by the LED renderer are retained."""

    remaining_percent: int
    primary_remaining_percent: int | None
    secondary_remaining_percent: int | None
    updated_monotonic: float


def _remaining_percent(window: Any) -> int | None:
    if window is None:
        return None
    if not isinstance(window, dict):
        raise QuotaError("invalid rate-limit window")
    used = window.get("usedPercent")
    if isinstance(used, bool) or not isinstance(used, int):
        raise QuotaError("invalid used percentage")
    if not 0 <= used <= 100:
        raise QuotaError("out-of-range used percentage")
    return 100 - used


def parse_quota_result(
    result: Any, *, updated_monotonic: float
) -> QuotaSnapshot:
    """Reduce an app-server response to the most restrictive remaining quota."""

    if not isinstance(result, dict):
        raise QuotaError("invalid rate-limit response")
    rate_limits: Any = None
    by_id = result.get("rateLimitsByLimitId")
    if isinstance(by_id, dict):
        rate_limits = by_id.get("codex")
    if rate_limits is None:
        rate_limits = result.get("rateLimits")
    if not isinstance(rate_limits, dict):
        raise QuotaError("missing Codex rate limits")

    primary = _remaining_percent(rate_limits.get("primary"))
    secondary = _remaining_percent(rate_limits.get("secondary"))
    available = [value for value in (primary, secondary) if value is not None]
    if not available:
        raise QuotaError("no Codex rate-limit window")
    return QuotaSnapshot(
        remaining_percent=min(available),
        primary_remaining_percent=primary,
        secondary_remaining_percent=secondary,
        updated_monotonic=updated_monotonic,
    )


class CodexQuotaSource:
    """Poll ``account/rateLimits/read`` without touching session or auth files."""

    def __init__(
        self,
        *,
        command: Sequence[str] = ("codex", "app-server", "--listen", "stdio://"),
        refresh_interval_s: float = 60.0,
        stale_after_s: float = 900.0,
        response_timeout_s: float = 15.0,
        monotonic: Callable[[], float] = time.monotonic,
    ) -> None:
        if not command or any(
            not isinstance(part, str) or not part for part in command
        ):
            raise ValueError("invalid Codex app-server command")
        if refresh_interval_s <= 0 or stale_after_s < refresh_interval_s:
            raise ValueError("invalid quota timing")
        if response_timeout_s <= 0:
            raise ValueError("invalid quota response timeout")
        self.command = tuple(command)
        self.refresh_interval_s = refresh_interval_s
        self.stale_after_s = stale_after_s
        self.response_timeout_s = response_timeout_s
        self.monotonic = monotonic
        self._snapshot: QuotaSnapshot | None = None
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._process: subprocess.Popen[str] | None = None
        self._last_error: str | None = None

    def start(self) -> None:
        if self._thread is not None and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._run, name="codex-quota-source", daemon=True
        )
        self._thread.start()

    def close(self) -> None:
        self._stop.set()
        with self._lock:
            process = self._process
        if process is not None and process.poll() is None:
            process.terminate()
        thread = self._thread
        if thread is not None:
            thread.join(timeout=2.0)
        if process is not None and process.poll() is None:
            process.kill()
        self._thread = None

    def current(self, *, now: float | None = None) -> QuotaSnapshot | None:
        now = self.monotonic() if now is None else now
        with self._lock:
            snapshot = self._snapshot
        if snapshot is None or now - snapshot.updated_monotonic > self.stale_after_s:
            return None
        return snapshot

    def status(self, *, now: float | None = None) -> str:
        now = self.monotonic() if now is None else now
        with self._lock:
            snapshot = self._snapshot
            last_error = self._last_error
        if snapshot is None:
            return "unavailable" if last_error else "starting"
        if now - snapshot.updated_monotonic > self.stale_after_s:
            return "stale"
        return "available"

    @staticmethod
    def _write_message(stdin: IO[str], message: dict[str, Any]) -> None:
        stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
        stdin.flush()

    def _read_response(
        self,
        messages: queue.Queue[str | None],
        request_id: int,
    ) -> dict[str, Any]:
        deadline = self.monotonic() + self.response_timeout_s
        while not self._stop.is_set():
            remaining = deadline - self.monotonic()
            if remaining <= 0:
                raise QuotaError("Codex app-server response timed out")
            try:
                line = messages.get(timeout=min(remaining, 0.5))
            except queue.Empty:
                continue
            if line is None:
                raise QuotaError("Codex app-server exited")
            try:
                message = json.loads(line)
            except (json.JSONDecodeError, UnicodeDecodeError):
                continue
            if not isinstance(message, dict) or message.get("id") != request_id:
                continue
            if "error" in message:
                # App-server errors can include upstream response bodies. Never
                # retain or log their text.
                raise QuotaError("Codex app-server request failed")
            result = message.get("result")
            if not isinstance(result, dict):
                raise QuotaError("Codex app-server returned no result")
            return result
        raise QuotaError("Codex quota source stopped")

    @staticmethod
    def _read_stdout(stdout: IO[str], messages: queue.Queue[str | None]) -> None:
        try:
            for line in stdout:
                messages.put(line)
        finally:
            messages.put(None)

    def _session(self) -> None:
        process = subprocess.Popen(
            self.command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        assert process.stdin is not None
        assert process.stdout is not None
        with self._lock:
            self._process = process
        messages: queue.Queue[str | None] = queue.Queue(maxsize=256)
        reader = threading.Thread(
            target=self._read_stdout,
            args=(process.stdout, messages),
            name="codex-quota-reader",
            daemon=True,
        )
        reader.start()
        try:
            self._write_message(
                process.stdin,
                {
                    "method": "initialize",
                    "id": 1,
                    "params": {
                        "clientInfo": {
                            "name": "unoq_codex_matrix",
                            "title": "UNO Q Codex Matrix",
                            "version": "0.1.0",
                        }
                    },
                },
            )
            self._read_response(messages, 1)
            self._write_message(process.stdin, {"method": "initialized"})

            request_id = 2
            while not self._stop.is_set():
                self._write_message(
                    process.stdin,
                    {"method": "account/rateLimits/read", "id": request_id},
                )
                result = self._read_response(messages, request_id)
                snapshot = parse_quota_result(
                    result, updated_monotonic=self.monotonic()
                )
                with self._lock:
                    self._snapshot = snapshot
                    self._last_error = None
                request_id += 1
                if self._stop.wait(self.refresh_interval_s):
                    break
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    process.kill()
            with self._lock:
                if self._process is process:
                    self._process = None

    def _run(self) -> None:
        retry_delay = min(30.0, self.refresh_interval_s)
        while not self._stop.is_set():
            try:
                self._session()
                if self._stop.is_set():
                    break
                raise QuotaError("Codex app-server session ended")
            except (OSError, BrokenPipeError, QuotaError, ValueError, TypeError) as exc:
                with self._lock:
                    self._last_error = type(exc).__name__
            if self._stop.wait(retry_delay):
                break
