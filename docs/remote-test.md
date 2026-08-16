# Remote lifecycle-Hook test

## Current result

**Awaiting phone-originated verification.** The investigated UNO Q now has
Codex 0.147.0, and the local CLI path has produced the expected lifecycle Hook
events. No person with the paired phone session was available to start a Remote
turn, so neither support nor incompatibility is claimed for that path.

This test requires a person with access to the phone session. It is intentionally
kept outside CI and must not block completion of Linux, firmware, or local Hook
work when that device is unavailable.

## Prerequisites

1. Install a lifecycle-Hook-capable Codex for the `arduino` user through the
   normal supported path. The investigated board used Codex 0.147.0.
2. Install this project and ensure the Hook is reviewed and trusted in `/hooks`.
3. Confirm the daemon, Router, and MCU are healthy:

   ```bash
   unoq-codex-matrix doctor
   unoq-codex-matrix status
   ```

4. Use a dedicated disposable smoke repository, not an existing project:

   ```bash
   mkdir -p /home/arduino/src/unoq-codex-matrix-smoke
   cd /home/arduino/src/unoq-codex-matrix-smoke
   git init
   printf '# Remote smoke test\n' > README.md
   ```

An initial commit is optional. Use the existing Git identity only if one is
already configured for this test; do not change global Git configuration solely
for the smoke repository.

## Test procedure

1. Record the pre-test aggregate output without identifiers:

   ```bash
   unoq-codex-matrix status
   ```

2. From the phone, start an ordinary Remote turn on the UNO Q and its smoke
   repository. A suitable task is:

   > Read README.md, create a small Python function with one unit test, run the
   > test once, and summarize the result.

   The prompt deliberately contains no instruction about a matrix, LED, Hook,
   daemon, or status display.

3. Observe the matrix without interacting with the task. The minimum expected
   sequence is:

   ```text
   THINKING -> READING or COMMAND -> WRITING -> TESTING -> THINKING -> SUCCESS -> IDLE
   ```

   Some transitions can be brief. Codex 0.147.0 may implement a file read with
   Bash, which is correctly classified COMMAND rather than READING. SUCCESS
   should remain for approximately eight seconds under the default
   configuration. Other concurrently active sessions may legitimately take
   priority.

4. Immediately after the turn completes, run:

   ```bash
   unoq-codex-matrix status
   journalctl -u unoq-codex-matrix.service --since '-5 min' --no-pager
   ```

   Confirm that `last hook event age` is recent, the service stayed healthy, and
   the journal contains no prompt, command, identifier, or response body. If the
   system unit has not yet been installed, omit the journal command and use the
   foreground daemon's aggregate health output without enabling raw debug data.

5. Wait at least eight seconds and confirm the aggregate returns to IDLE when no
   other session is active.

6. Run one local Codex turn with a similarly ordinary task. Compare only the
   categories and timing, not content, to confirm local and Remote paths produce
   the same normalized behavior.

## Optional state coverage

- Use `unoq-codex-matrix demo` to visually check WAITING and all other states
  without forcing a risky permission request.
- Exercise an intentional test failure only inside the disposable repository.
  ERROR should be brief only when Codex supplies an explicit structured failure
  field. Codex 0.147.0 exposed the tested Bash response as a string, so the local
  reference run showed TESTING followed by THINKING without ERROR. The project
  deliberately does not search the response body.
- If the installed Codex supports subagents, use one bounded read-only subtask
  and confirm SUBAGENT followed by THINKING. Do not require it when unavailable.

## Pass criteria

- A phone-originated turn updates `last hook event age`.
- Expected lifecycle states appear without any display instruction in the task.
- The Hook does not delay, fail, or alter the Codex turn.
- SUCCESS expires to IDLE, and a daemon/Router error does not expose content.
- The journal and any retained fixture pass the privacy rules.

Record the Codex version, Remote path, events seen, absent events, timestamp, and
test outcome in this document or release evidence. Do not record the prompt,
answer, raw Hook payload, IDs, transcript, working path, or command body.

## If no Remote Hook fires

Treat that result as a version/path compatibility limitation, not a reason to
read unstable session files. Recheck `/hooks`, Hook enablement, trust, user
identity, daemon health, and whether a local turn fires the same configuration.
If local works and Remote still does not, document the exact Codex version and
sanitized observation as “Remote Hook verification failed/pending.”

Do not silently enable JSONL parsing, browser scraping, App Server attachment,
or raw debug capture. `AppServerSource` may be designed later only after a
side-effect-free additional client is demonstrated on the real runtime.
