# Hook fixtures

No raw hook payload is stored here. `codex-0.147.0-uno-q.jsonl` was captured on
an AArch64 UNO Q and reduced to the probe allowlist before it entered version
control. Hashes cover only the first eight identifier characters; no identifier,
prompt, response body, command, path, transcript, or environment value is kept.

This Codex build emitted `tool_response` as a string for both Bash and
`apply_patch`. Protocol v1 never searches response text for words such as
"error", so those responses alone are not treated as explicit failures.
