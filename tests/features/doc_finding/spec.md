# doc_finding — the shared doc-lint finding shape

Contract for `src/docfinding.{h,cpp}`. Full design: `docs/specs/ANTS-3664.md`.

`DocFinding::Finding` is the one shape the doc-lint verbs
(ANTS-3660 `doc_dedup`, ANTS-3661 `doc_symbols`, ANTS-3662 `spec_lint`)
emit natively and that ANTS-3663 `doc_lint` composes. It exists so those
three do not each invent a shape, and so the composer normalises nothing.

## Invariants under test

- **INV-1** — `toJson` always emits the five value keys; `line` is emitted
  even when `0`; `auto_fixable` only when true; `emission_index` never.
  *Test:* `DocFinding.Inv1JsonKeysAndOmission`,
  `DocFinding.Inv1ListOverloadPreservesOrder`.
- **INV-2** — `countsByVerbAndKind` counts every finding it is handed,
  nesting `verb` then `kind`, asserted by value.
  *Test:* `DocFinding.Inv2CountsByVerbAndKind`.
- **INV-3** — wire keys are snake_case while members are camelCase.
  *Test:* `DocFinding.Inv3WireNamesAreSnakeCase`.
- **INV-4** — `src/docfinding.h` includes exactly four Qt headers, all
  QtCore, asserted as an allow-list by set equality.
  *Test:* `DocFinding.Inv4CoreOnlyHeader`.
- **INV-5** — every checker engine takes its document as text, never as a
  path it opens. *Test:* `DocFinding.Inv5EnginesTakeTextNotPaths` —
  **currently skipped**, see below.

## Two rows that carry the weight

**`line:0` is emitted, not omitted.** `0` means document scope (a missing
required section has no line), so an omit-when-false serialiser would make
a document-level finding indistinguishable from a malformed one. This is
the opposite rule to `auto_fixable`, three lines away in the same
function, which is exactly why both are asserted.

**`emission_index` is never on the wire.** It orders findings within one
run and is unstable across runs — the same document reviewed with a
different `checks[]` set renumbers identical findings. This is the line an
obvious serialiser adds, and it reached a sibling spec's `fixed[]` array
before cold-eyes loop 4 caught it, so the assertion is against a mistake
that has actually been made rather than a hypothetical one.

## Why INV-5 skips rather than passes

The scrape targets `src/docdedup.cpp`, `src/docsymbols.cpp` and
`src/speclint.cpp`, none of which exist until ANTS-3660/3661/3662 land. A
scrape over absent files finds no violations and reports green, which is
false assurance of the precise kind this suite is meant to prevent. It
skips with the unblocking condition named, and turns real with the first
engine.

## Verified RED

The type is new, so the honest RED is narrower than usual: with
`src/docfinding.h` absent the test does not compile, which proves nothing
about the assertions. The assertions were instead proven by **mutation**
after the implementation landed — `toJson` was made to emit
`emission_index`, and `DocFinding.Inv1JsonKeysAndOmission` turned red.
That is the row most likely to be got wrong, and it is now known to be
falsifiable rather than merely passing.
