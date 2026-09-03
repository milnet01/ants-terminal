# changelog_log / changelog_query nested `path` (ANTS-4812)

## Context

`changelog_log` resolves its target with `findChangelogUnder`, which walks UP
from `caller_cwd` to the repo boundary. A project shipping a second changelog
for a separately-versioned bundled component — Vestige's
`tools/audit/CHANGELOG.md` beside its root one — therefore could not reach the
nested file at all: a nested `caller_cwd` resolves back to the root changelog.

The fallback was a raw edit, which is the route this verb exists to replace.
What goes missing there is the atomic write, category routing, format
validation and the `[Unreleased]` guard — and it goes missing on precisely the
file most likely to drift, because a component changelog is edited least often
and by fewest people. The reporting project's nested file had already diverged
in format, by hand.

## Contract

`changelog_log` and `changelog_query` take an optional `path`: the changelog to
act on, project-relative.

- Absent `path` is the previous behaviour exactly — discovery from
  `caller_cwd`.
- `path` names an **existing** changelog. It never creates one.
- Resolution reuses `PathValidation::validatePath` with
  `allowOutsideRoot=false`, the same shape `spec_log`'s `path` uses
  (ANTS-1295).
- The existing Markdown/YAML probe still runs on whatever file is handed over,
  so a nested file in a convention this writer cannot parse refuses
  `format_mismatch` as it does today rather than being written wrongly.

Both verbs take it, because the read side cannot check what the write side just
wrote if only one of them can be aimed.

## Invariants

- **INV-1** — `path` writes to the nested changelog and leaves the root one
  untouched.
  *Test:* `Inv1PathWritesNestedNotRoot`. **Fails against the pre-fix verb**,
  which ignores `path` and writes the root file.

- **INV-2** — `changelog_query` with the same `path` reads back that write,
  **from the nested file**. The two changelogs carry different marker entries
  and the test asserts on both: without that, the invariant passes vacuously
  against the pre-fix verb, where write and read both fall back to the root
  file and the round-trip succeeds while reading the wrong changelog. Measured
  — the first draft of this test passed in the red state for exactly that
  reason.
  *Test:* `Inv2QueryReadsTheSameNestedFile`. **Fails against the pre-fix verb.**

- **INV-3** — a root-escaping `path` refuses `bad_path`, and the root changelog
  is left byte-identical.
  *Test:* `Inv3RootEscapingPathRefuses`.

- **INV-4** — a `path` naming no existing file refuses `no_changelog` and
  creates nothing.
  *Test:* `Inv4NonExistentPathRefusesAndCreatesNothing`.

- **INV-5** — an absent `path` still resolves the root changelog by discovery,
  and does not touch the nested one. This is what keeps every existing caller
  byte-identical.
  *Test:* `Inv5AbsentPathStillWritesRoot`. Passes before and after.

## Deliberately not covered

`op:"release"` and the other ops against a nested file. They share the one
resolution point this change touches, so covering `add` and the query pins the
routing; the ops' own behaviour is their existing tests'.
