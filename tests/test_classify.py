import pytest

from unoq_codex_matrix.classify import classify_bash, classify_tool, explicit_failure
from unoq_codex_matrix.protocol import State


@pytest.mark.parametrize(
    ("command", "expected"),
    [
        ("arduino-cli upload -b arduino:zephyr:unoq", State.FLASHING),
        ("sudo /usr/bin/openocd -f board.cfg", State.FLASHING),
        ("west flash", State.FLASHING),
        ("dfu-util -D firmware.bin", State.FLASHING),
        ("python3 -m pytest -q", State.TESTING),
        ("npm test", State.TESTING),
        ("npm run test", State.TESTING),
        ("pnpm test", State.TESTING),
        ("yarn test", State.TESTING),
        ("cargo test --workspace", State.TESTING),
        ("go test ./...", State.TESTING),
        ("ctest --output-on-failure", State.TESTING),
        ("meson test -C build", State.TESTING),
        ("make -j2 test", State.TESTING),
        ("npx vitest run", State.TESTING),
        ("jest --runInBand", State.TESTING),
        ("arduino-cli compile -b arduino:zephyr:unoq firmware", State.BUILDING),
        ("cmake --build build", State.BUILDING),
        ("ninja -C build", State.BUILDING),
        ("make -j4", State.BUILDING),
        ("cargo build --release", State.BUILDING),
        ("npm run build", State.BUILDING),
        ("pnpm run build", State.BUILDING),
        ("yarn run build", State.BUILDING),
        ("vite build", State.BUILDING),
        ("git status", State.COMMAND),
    ],
)
def test_bash_classification(command: str, expected: State) -> None:
    assert classify_bash(command) is expected


def test_compound_command_uses_highest_operational_state() -> None:
    assert classify_bash("cmake --build build && pytest -q | tee result.txt") is State.TESTING
    assert (
        classify_bash("pytest -q && arduino-cli upload -b arduino:zephyr:unoq")
        is State.FLASHING
    )


def test_quotes_and_malformed_input_fall_back_safely() -> None:
    assert classify_bash("echo 'pytest && arduino-cli upload'") is State.COMMAND
    assert classify_bash("echo 'unterminated") is State.COMMAND
    assert classify_bash(None) is State.COMMAND


def test_shell_c_and_environment_wrappers_are_understood() -> None:
    assert classify_bash("env CI=1 bash -lc 'python -m pytest -q'") is State.TESTING
    assert classify_bash("MODE=release sudo -u arduino ninja -C build") is State.BUILDING


@pytest.mark.parametrize(
    ("tool", "tool_input", "expected"),
    [
        ("apply_patch", {"patch": "secret diff"}, State.WRITING),
        ("Write", {"content": "secret file"}, State.WRITING),
        ("mcp__filesystem__read_file", {"path": "/private/path"}, State.READING),
        ("Grep", {"pattern": "token"}, State.READING),
        ("spawn_agent", {"message": "private task"}, State.SUBAGENT),
        ("Bash", {"command": "pytest"}, State.TESTING),
        ("functions.exec_command", {"cmd": "arduino-cli compile"}, State.BUILDING),
        ("unknown_tool", {}, State.COMMAND),
    ],
)
def test_tool_classification(tool: str, tool_input: object, expected: State) -> None:
    assert classify_tool(tool, tool_input) is expected


@pytest.mark.parametrize(
    "response",
    [
        {"exit_code": 1},
        {"returnCode": "2"},
        {"status": "failed"},
        {"success": False},
        {"is_error": True},
        {"error": {"code": "tool_failed"}},
        {"metadata": {"exit_code": 7}},
    ],
)
def test_only_structured_failure_signals_are_recognized(response: object) -> None:
    assert explicit_failure(response)


@pytest.mark.parametrize(
    "response",
    [
        "error: tests failed",
        {"output": "fatal error and failed"},
        {"exit_code": 0, "status": "completed", "success": True},
        {"error": ""},
        None,
    ],
)
def test_free_form_error_words_do_not_mark_failure(response: object) -> None:
    assert not explicit_failure(response)
