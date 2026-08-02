# spec_field_extent — test contract

Behavioural conformance for the markdown header-field extent rule
(ANTS-3785, which also covers ANTS-3672). Drives the pure
`SpecParse::headerField` transform directly, plus `SpecParse::parseSpecBody`
for the reader half. The writer half lives in
`tests/features/mcp_spec_log/`, so every `setStatus` assertion stays in one
place.

A spec's `**Field:**` header value may wrap onto continuation lines — 49 of
the 172 specs carrying a Status field do. Before this change the reader
truncated such a value at its first physical line and the writer orphaned
the rest, because each modelled the field as one line.

Invariants exercised (docs/specs/ANTS-3785-header-field-extent.md §3 / §6):

- T1 (INV-1) — extent terminates on each of the four: a blank line, a
  further `**Field:**` marker, an ATX heading, and end of input. One
  fixture per terminator, asserting `lineCount`.
- T2 (INV-2) — a continuation line beginning with a list bullet belongs to
  the field, and the joined value is the opener's trailing text plus each
  continuation, stripped and joined with exactly one space. Fixture is a
  byte-for-byte copy of `docs/specs/ANTS-1436.md`'s header, checked in so a
  later edit to that live spec cannot move the expectation.
- T3 (INV-3) — `parseSpecBody` returns the whole joined value for `Status`
  and `Kind`, not the first-line prefix.
- T4 (INV-8) — `Kind` is governed by the same rule as `Status`. Synthetic
  fixture: the corpus's only wrapped `Kind` is also its inline-marker case,
  and a fixture exercising two rules at once proves neither.
- T5 (INV-9) — a `**Field:**` marker only opens or closes a field at the
  start of a line; an inline bold colon-run is value text. Fixture is the
  `docs/specs/ANTS-1253.md` shape (`**Kind:** … **Lanes:** …` on one
  physical line plus a continuation).
- T6 (INV-10, reader leg) — the search is bounded to the header block, so a
  `**Status:**` inside a fenced example below the first `## ` heading is
  never matched and `headerField` reports absent.
- T7 (INV-6) — source scrape: `src/speclog.cpp` and `src/specparse.cpp`
  each contain `headerField(`, and neither contains `statusRe` or `kindRe`.
  The positive half is load-bearing — an absence-only assertion passes
  against a hand-rolled `startsWith("**Status:**")` that never adopted the
  shared rule, which is the build this invariant exists to reject.

Not covered here (see `tests/features/mcp_spec_log/`): INV-4 (writer
replaces the whole extent), INV-5 (byte fidelity + 1-based returned line),
INV-7 (refusals unchanged), INV-10's writer leg.

Label: `features`. Compiled into the `test_claude` bundle's `SOURCES` list,
not as a standalone `add_executable` (see `tests/features/README.md`).
