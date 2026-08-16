"""Privacy-preserving Codex tool classification.

Command text is inspected only in memory.  Callers receive a coarse state and
must never attach the original command to the normalized event or logs.
"""

from __future__ import annotations

import os
import re
import shlex
from collections.abc import Mapping, Sequence
from typing import Final

from .protocol import State


_ASSIGNMENT: Final = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")
_BOUNDARY_CHARS: Final = frozenset(";&|()")
_SHELLS: Final = frozenset({"bash", "dash", "fish", "ksh", "sh", "zsh"})


def _basename(token: str) -> str:
    # Commands originate on Linux, but accepting either separator makes the
    # pure classifier deterministic on every development platform.
    return os.path.basename(token.replace("\\", "/")).lower().removesuffix(".exe")


def _tokenize(command: str) -> list[str] | None:
    if not isinstance(command, str) or not command.strip():
        return []
    try:
        lexer = shlex.shlex(
            command.replace("\r\n", "\n").replace("\r", "\n").replace("\n", " ; "),
            posix=True,
            punctuation_chars=";&|()",
        )
        lexer.whitespace_split = True
        lexer.commenters = ""
        return list(lexer)
    except (TypeError, ValueError):
        # An unmatched quote or another malformed shell fragment is not a
        # reason to guess.  COMMAND is the safe fallback.
        return None


def _segments(tokens: Sequence[str]) -> list[list[str]]:
    result: list[list[str]] = []
    current: list[str] = []
    for token in tokens:
        if token and set(token) <= _BOUNDARY_CHARS:
            if current:
                result.append(current)
                current = []
        else:
            current.append(token)
    if current:
        result.append(current)
    return result


def _unwrap(segment: Sequence[str]) -> tuple[str, list[str]]:
    tokens = list(segment)
    index = 0
    while index < len(tokens) and _ASSIGNMENT.match(tokens[index]):
        index += 1

    while index < len(tokens):
        executable = _basename(tokens[index])
        if executable == "sudo":
            index += 1
            while index < len(tokens) and tokens[index].startswith("-"):
                option = tokens[index]
                index += 1
                if option in {"-C", "-D", "-g", "-h", "-p", "-R", "-T", "-u"}:
                    index += 1
            continue
        if executable == "env":
            index += 1
            while index < len(tokens) and (
                tokens[index].startswith("-") or _ASSIGNMENT.match(tokens[index])
            ):
                index += 1
            continue
        if executable in {"command", "exec", "nohup", "time"}:
            index += 1
            while index < len(tokens) and tokens[index].startswith("-"):
                index += 1
            continue
        return executable, tokens[index + 1 :]
    return "", []


def _has_target(arguments: Sequence[str], target: str) -> bool:
    return any(arg.lower() == target for arg in arguments if not arg.startswith("-"))


def _command_state(executable: str, arguments: Sequence[str]) -> State | None:
    args = [argument.lower() for argument in arguments]

    if executable == "arduino-cli" and _has_target(args, "upload"):
        return State.FLASHING
    if executable in {"remoteocd", "openocd", "dfu-util", "bossac"}:
        return State.FLASHING
    if executable == "west" and _has_target(args, "flash"):
        return State.FLASHING

    if executable in {"pytest", "pytest-3", "ctest", "jest", "vitest"}:
        return State.TESTING
    if (executable.startswith("python") or executable.startswith("pypy")) and any(
        args[index : index + 2] == ["-m", "pytest"] for index in range(max(0, len(args) - 1))
    ):
        return State.TESTING
    if executable == "npm" and (args[:1] == ["test"] or args[:2] == ["run", "test"]):
        return State.TESTING
    if executable in {"pnpm", "yarn"} and (
        args[:1] == ["test"] or args[:2] == ["run", "test"]
    ):
        return State.TESTING
    if executable in {"cargo", "go", "meson"} and args[:1] == ["test"]:
        return State.TESTING
    if executable == "make" and _has_target(args, "test"):
        return State.TESTING
    if executable in {"npx", "pnpx"} and args and _basename(args[0]) in {"jest", "vitest"}:
        return State.TESTING

    if executable == "arduino-cli" and _has_target(args, "compile"):
        return State.BUILDING
    if executable == "cmake" and "--build" in args:
        return State.BUILDING
    if executable in {"ninja", "make"}:
        return State.BUILDING
    if executable == "cargo" and args[:1] == ["build"]:
        return State.BUILDING
    if executable in {"npm", "pnpm", "yarn"} and args[:2] == ["run", "build"]:
        return State.BUILDING
    if executable == "vite" and args[:1] == ["build"]:
        return State.BUILDING
    if executable in {"npx", "pnpx"} and args[:2] == ["vite", "build"]:
        return State.BUILDING
    return None


def classify_bash(command: object, *, _depth: int = 0) -> State:
    """Classify a shell command without retaining or returning its text.

    ``shlex`` handles quoting and command separators.  This deliberately is not
    a complete shell parser: malformed or ambiguous input falls back to COMMAND.
    If a compound command contains multiple known activities, the most
    operationally important state wins (flashing, then testing, then building).
    """

    if not isinstance(command, str):
        return State.COMMAND
    tokens = _tokenize(command)
    if tokens is None:
        return State.COMMAND

    observed: set[State] = set()
    for segment in _segments(tokens):
        executable, arguments = _unwrap(segment)
        if not executable:
            continue
        if executable in _SHELLS and _depth < 1:
            try:
                command_index = next(
                    index for index, argument in enumerate(arguments) if argument in {"-c", "-lc"}
                )
            except StopIteration:
                pass
            else:
                if command_index + 1 < len(arguments):
                    observed.add(classify_bash(arguments[command_index + 1], _depth=_depth + 1))
                    continue
        state = _command_state(executable, arguments)
        if state is not None:
            observed.add(state)

    for state in (State.FLASHING, State.TESTING, State.BUILDING):
        if state in observed:
            return state
    return State.COMMAND


def _command_from_input(tool_input: object) -> object:
    if isinstance(tool_input, str):
        return tool_input
    if not isinstance(tool_input, Mapping):
        return None
    for key in ("command", "cmd", "script"):
        value = tool_input.get(key)
        if isinstance(value, str):
            return value
        if isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray)):
            if all(isinstance(item, str) for item in value):
                return "\n".join(value)
    return None


def classify_tool(tool_name: object, tool_input: object = None) -> State:
    """Map a Codex tool name and ephemeral input to a coarse display state."""

    if not isinstance(tool_name, str):
        return State.COMMAND
    normalized = re.sub(r"[^a-z0-9]+", "_", tool_name.lower()).strip("_")
    leaf = normalized.rsplit("_", 1)[-1]

    if normalized in {"bash", "shell", "exec_command", "run_command"} or normalized.endswith(
        ("_exec_command", "_run_command")
    ):
        return classify_bash(_command_from_input(tool_input))
    if normalized in {
        "apply_patch",
        "edit",
        "multiedit",
        "notebookedit",
        "write",
        "write_file",
        "create_file",
    } or leaf in {"edit", "write"} or normalized.endswith(
        ("_apply_patch", "_write_file", "_create_file")
    ):
        return State.WRITING
    if normalized in {
        "read",
        "read_file",
        "grep",
        "glob",
        "search",
        "find",
        "list_directory",
        "list_files",
        "search_query",
    } or leaf in {"read", "grep", "glob", "search", "find"} or normalized.endswith(
        ("_read_file", "_search_query", "_list_directory", "_list_files")
    ):
        return State.READING
    if normalized in {"agent", "spawn_agent", "subagent_start"} or normalized.endswith(
        ("_spawn_agent", "_subagent_start")
    ):
        return State.SUBAGENT
    return State.COMMAND


def _meaningful_error(value: object) -> bool:
    return value not in (None, False, "", (), [], {})


def explicit_failure(tool_response: object) -> bool:
    """Return true only for structured, explicit failure information.

    Free-form response text is intentionally never searched for words such as
    ``error``.  Only well-known structured fields are inspected.
    """

    if not isinstance(tool_response, Mapping):
        return False

    for key in ("exit_code", "exitCode", "return_code", "returnCode"):
        if key not in tool_response:
            continue
        value = tool_response[key]
        if isinstance(value, bool):
            continue
        try:
            if int(value) != 0:
                return True
        except (TypeError, ValueError):
            pass

    status = tool_response.get("status")
    if isinstance(status, str) and status.strip().lower() in {"failed", "failure", "error"}:
        return True
    if tool_response.get("success") is False:
        return True
    if tool_response.get("is_error") is True or tool_response.get("isError") is True:
        return True
    for key in ("error", "tool_error", "toolError"):
        if key in tool_response and _meaningful_error(tool_response[key]):
            return True

    # Some Codex versions place process metadata in a dedicated mapping.  Do
    # not recurse into arbitrary result/content objects, which may be user data.
    metadata = tool_response.get("metadata")
    return isinstance(metadata, Mapping) and explicit_failure(metadata)
