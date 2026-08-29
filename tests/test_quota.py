from __future__ import annotations

import json
from pathlib import Path
import queue
import sys
import time

import pytest

from unoq_codex_matrix.quota import CodexQuotaSource, QuotaError, parse_quota_result


def test_parser_uses_most_restrictive_codex_window() -> None:
    snapshot = parse_quota_result(
        {
            "rateLimits": {
                "primary": {"usedPercent": 25},
                "secondary": {"usedPercent": 68},
            }
        },
        updated_monotonic=123.0,
    )

    assert snapshot.remaining_percent == 32
    assert snapshot.primary_remaining_percent == 75
    assert snapshot.secondary_remaining_percent == 32
    assert snapshot.updated_monotonic == 123.0


def test_parser_prefers_codex_bucket_from_multi_bucket_response() -> None:
    snapshot = parse_quota_result(
        {
            "rateLimits": {"primary": {"usedPercent": 99}},
            "rateLimitsByLimitId": {
                "codex": {"primary": {"usedPercent": 7}},
                "other": {"primary": {"usedPercent": 100}},
            },
        },
        updated_monotonic=1.0,
    )

    assert snapshot.remaining_percent == 93


@pytest.mark.parametrize(
    "result",
    [
        {},
        {"rateLimits": {}},
        {"rateLimits": {"primary": {"usedPercent": True}}},
        {"rateLimits": {"primary": {"usedPercent": -1}}},
        {"rateLimits": {"primary": {"usedPercent": 101}}},
    ],
)
def test_parser_rejects_missing_or_invalid_percentages(result: object) -> None:
    with pytest.raises(QuotaError):
        parse_quota_result(result, updated_monotonic=0.0)


def test_source_hides_stale_snapshot() -> None:
    now = [10.0]
    source = CodexQuotaSource(
        refresh_interval_s=30,
        stale_after_s=60,
        monotonic=lambda: now[0],
    )
    source._snapshot = parse_quota_result(  # noqa: SLF001 - bounded state test
        {"rateLimits": {"primary": {"usedPercent": 40}}},
        updated_monotonic=10.0,
    )

    assert source.current() is not None
    assert source.status() == "available"
    now[0] = 70.1
    assert source.current() is None
    assert source.status() == "stale"


def test_app_server_error_body_is_not_propagated() -> None:
    source = CodexQuotaSource(refresh_interval_s=30, stale_after_s=60)
    messages: queue.Queue[str | None] = queue.Queue()
    messages.put(
        json.dumps(
            {
                "id": 7,
                "error": {
                    "message": "upstream body with private@example.test and account-123"
                },
            }
        )
    )

    with pytest.raises(QuotaError) as caught:
        source._read_response(messages, 7)  # noqa: SLF001 - privacy boundary test

    assert "private@example.test" not in str(caught.value)
    assert "account-123" not in str(caught.value)


def test_source_polls_jsonl_app_server(tmp_path: Path) -> None:
    server = tmp_path / "fake_app_server.py"
    server.write_text(
        """
import json
import sys

for line in sys.stdin:
    message = json.loads(line)
    if message.get("method") == "initialize":
        print(json.dumps({"id": message["id"], "result": {"ready": True}}), flush=True)
    elif message.get("method") == "account/rateLimits/read":
        print(json.dumps({
            "id": message["id"],
            "result": {"rateLimits": {
                "primary": {"usedPercent": 10},
                "secondary": {"usedPercent": 45}
            }}
        }), flush=True)
""".lstrip(),
        encoding="utf-8",
    )
    source = CodexQuotaSource(
        command=(sys.executable, "-u", str(server)),
        refresh_interval_s=0.05,
        stale_after_s=1.0,
        response_timeout_s=1.0,
    )

    source.start()
    try:
        deadline = time.monotonic() + 2.0
        snapshot = None
        while snapshot is None and time.monotonic() < deadline:
            snapshot = source.current()
            time.sleep(0.01)
        assert snapshot is not None
        assert snapshot.remaining_percent == 55
        assert source.status() == "available"
    finally:
        source.close()
