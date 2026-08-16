#!/usr/bin/env python3
"""Small dependency-free release scan for obvious credentials and host data."""

from __future__ import annotations

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
SKIP_PARTS = {".git", ".venv", "__pycache__", ".pytest_cache", "*.egg-info"}
MAX_SCAN_BYTES = 5 * 1024 * 1024
PATTERNS = {
    "private key": re.compile(rb"-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----"),
    "OpenAI-style secret": re.compile(rb"\bsk-[A-Za-z0-9_-]{20,}\b"),
    "GitHub token": re.compile(rb"\bgh[pousr]_[A-Za-z0-9]{20,}\b"),
    "credential assignment": re.compile(
        rb"(?i)\b(?:api[_-]?key|access[_-]?token|password)\s*[:=]\s*['\"]?[A-Za-z0-9_./+=-]{16,}"
    ),
    "Windows user path": re.compile(rb"(?i)[A-Z]:\\Users\\[^\\\s]+"),
    "private IPv4 address": re.compile(
        rb"(?<!\d)(?:10\.\d{1,3}\.\d{1,3}\.\d{1,3}|192\.168\.\d{1,3}\.\d{1,3}|172\.(?:1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3})(?!\d)"
    ),
}


def ignored(path: Path) -> bool:
    for part in path.relative_to(ROOT).parts:
        if part in SKIP_PARTS or part.endswith(".egg-info"):
            return True
    return False


def main() -> int:
    findings: list[str] = []
    for path in ROOT.rglob("*"):
        if not path.is_file() or ignored(path):
            continue
        try:
            if path.stat().st_size > MAX_SCAN_BYTES:
                continue
            data = path.read_bytes()
        except OSError:
            continue
        for label, pattern in PATTERNS.items():
            if pattern.search(data):
                findings.append(f"{path.relative_to(ROOT)}: {label}")
    if findings:
        print("Potential secret or private host data found:", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print("Secret scan passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
