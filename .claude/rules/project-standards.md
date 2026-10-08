---
paths:
  - "docs/standards/**"
  - "tools/check-standard-mirrors.sh"
---

## Project standards

- **The shared standards are owned at `~/.claude/standards/`.**
  `coding.md`, `documentation.md`, `testing.md` and `commits.md` in
  `docs/standards/` are deltas: project rules first, then a verbatim mirror of
  the owner below a divider. `security.md` is the mirror alone.
- **Never edit a mirrored half.** Fix the owner, then run
  `tools/check-standard-mirrors.sh --write`. `tools/hooks/pre-commit` refuses a
  drifted mirror.
- **Check the owner is committed before `--write`:**
  `git -C ~/.claude status --porcelain`. If it is dirty, leave the mirror and
  commit with `ANTS_PRECOMMIT_NO_MIRRORS=1`, saying so in the message.
- **In the drift diff, `<` is the owner and `>` is the mirror.** Never push
  mirror text upstream.
- Two files are not deltas. `roadmap-format.md`: this project is upstream of
  the global copy, and this one governs. `specs.md`: a full standard owning a
  spec's shape; global `spec-format.md` § 1 owns whether a spec is needed.
- `docs/standards/` is the roster: every file there binds unless it marks
  itself superseded. `mcp-error-codes.md` is the refusal taxonomy
  (`mcp-errors.md` is superseded). `dependencies.md`: a below-latest pin needs
  a Downgrade Ledger row.
- Do not cite a delta file's section number from memory. Open the file.
- ADRs: `docs/decisions/` (Nygard). Specs: `docs/specs/`. Phase outcomes:
  `docs/journal/`. `docs/plans/` is historical only.
