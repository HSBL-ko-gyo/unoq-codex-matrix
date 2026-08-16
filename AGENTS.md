# Repository guidance

- Keep lifecycle input, state aggregation, Router RPC, and MCU rendering as
  separate layers.
- Treat hook input as sensitive and retain only the documented allowlist.
- Keep the hook fail-open, dependency-light, and non-blocking.
- Preserve the numeric state IDs in `docs/protocol.md`.
- Do not copy code from prior-art projects; consult `THIRD_PARTY.md` first.
- Run the privacy, protocol-consistency, and unit tests before publishing.
