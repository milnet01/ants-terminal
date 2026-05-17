# Feature spec: ANTS-1410 spec-anchor drift detection

The `/audit` skill lives at `~/.claude/skills/audit/SKILL.md` —
**outside this repo**. A direct source-scrape of the global skill
file would couple the project's CI to one developer's HOME-side
install, which is bad portability. Spec § Tests calls out this
limitation and accepts the trade-off.

This test acts as a **lower-bound drift detector**: it scrapes
`docs/specs/ANTS-1410.md` itself for the load-bearing anchors the
shipped skill MUST contain. If a refactor removes "step 1.5" or
the INV-N markers, the test breaks loudly. The actual skill
behaviour is verified manually per § Tests (b) — the recipe at the
bottom of the spec.

## Invariants

- **INV-1 / Spec is present.** `docs/specs/ANTS-1410.md` exists
  and is readable.
- **INV-2 / Step 1.5 anchor present.** The string
  `"Enumerate project-local CI gates"` appears in the spec.
- **INV-3 / All 7 spec INVs documented.** INV-1 through INV-7
  appear in the spec body (anchor strings `**INV-N /`).
- **INV-4 / Exclusion lists present.** The spec names the binary-
  name exclusion table (`ruff`, `bandit`, `cppcheck`, ...) and
  the formatter exclusion list (`black`, `prettier`,
  `clang-format`).
- **INV-5 / Pair-spec ref to ANTS-1351 present.** The spec
  documents this as the stopgap until ANTS-1351 lands.
