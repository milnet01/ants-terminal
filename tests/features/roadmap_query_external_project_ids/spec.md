# Feature spec: `roadmap_query` recognises non-Ants stable IDs (ANTS-1405)

`RoadmapDialog::parseBullets` previously hardcoded
`\[ANTS-(\d+)\]` and emitted `rec.id = "ANTS-" + captured(1)`. A
Claude Code instance running on the MAME Curator project — which
follows `docs/standards/roadmap-format.md` § 3.5.1's shareable
`[PROJ-NNNN]` shape — observed every bullet returning empty `id`,
forcing it to read `ROADMAP.md` directly to recover IDs (the cost
the MCP verb was added to avoid).

The widened regex is
`\[([A-Za-z][A-Za-z0-9_-]*-\d+)\]` and `rec.id` is assigned the
full capture group verbatim. For `[ANTS-NNNN]` the value is
byte-identical to pre-fix behaviour. For external projects, the
literal token (`MAME-CURATOR-42`, `mame-curator-7`, `MYPRJ-0042`)
populates the field.

## Invariants

- **INV-1 / `[ANTS-NNNN]` back-compat.** A bullet body containing
  `[ANTS-1234]` yields `rec.id == "ANTS-1234"`. Source anchor:
  `ANTS-1405` in `src/roadmapdialog.cpp::parseBullets`.
- **INV-2 / uppercase external.** `[MAME-CURATOR-42]` yields
  `rec.id == "MAME-CURATOR-42"`. Full token captured verbatim.
- **INV-3 / lowercase external.** `[mame-curator-7]` yields
  `rec.id == "mame-curator-7"`. Tolerant of real-world lowercase
  projects.
- **INV-4 / reject digit-leading.** `[42-bad-1]` does not match.
  `rec.id` stays empty (or falls back to `boldId` path).
- **INV-5 / no-dash bracket id is adopted (superseded by ANTS-1987).**
  `[FOO123]` is not matched by the body-wide `rxId` (no `-<digits>`
  tail), but the ANTS-1987 head-anchored extractor adopts an ID-shaped
  leading bracket right after the status emoji, so `rec.id == "FOO123"`.
  The original "must stay empty" contract was reversed by user decision
  2026-06-06 — the leading-bracket slot is the id slot by convention, and
  real projects (Vestige: `[Cl9]`/`[CE18]`) author dash-less short ids
  there. Mid-prose `[text]` is still ignored (head-anchored only).
- **INV-6 / single-letter prefix permitted.** `[R-5]` matches and
  yields `rec.id == "R-5"`. The "4–6 letters" rule in § 3.5.1 is
  documentary, not enforced on the read path.
- **INV-7 / underscore inside prefix permitted.** `[my_proj-9]`
  yields `rec.id == "my_proj-9"`.
- **INV-8 / first-match-wins.** A body with two bracketed tokens
  (e.g. `[ANTS-1234]` then `[mame-curator-7]`) resolves to the
  first — `rxId.match()` returns the leftmost capture.
- **INV-9 / MCP descriptor cites the standard.** The
  `roadmap_query` tool description (in
  `src/claudeintegration.cpp`) mentions `roadmap-format.md`
  § 3.5.1 and gives the `[PROJ-NNNN]` shape so Claude knows what
  `bullets[].id` will look like cross-project.
