# Repository guidance

- Keep lifecycle input, state aggregation, Router RPC, and MCU rendering as
  separate layers.
- Treat hook input as sensitive and retain only the documented allowlist.
- Keep the hook fail-open, dependency-light, and non-blocking.
- Preserve the numeric state IDs in `docs/protocol.md`.
- Do not copy code from prior-art projects; consult `THIRD_PARTY.md` first.
- Run the privacy, protocol-consistency, and unit tests before publishing.
- Keep README content to durable user-facing behavior, not current project
  status.
- Do not put temporary validation status, pending work, current test counts,
  benchmark results, CI run IDs, version-specific observations, or later-TODO
  notes in README files.
- Put time-specific validation in releases, changelogs, issues, or
  `docs/validation-history.md` according to its purpose.
- Do not duplicate implementation constants in README when source code or
  `docs/protocol.md` is canonical.
