# Feature: `doc_symbols` engine — resolve the identifiers a doc claims exist (ANTS-3661)

## Problem

`/cold-eyes` § 1e calls symbol resolution "the highest-yield check in the
pre-pass" and names the corpus's two worst findings: a function documented as
returning something it did not, under a heading reading *"Verified API basis"*,
and a test seam a later reviewer's notes call *"fictional"*. Both were written
from recall; both die at a symbol lookup. Nothing did the lookup.

## Contract

`DocSymbols::scan(text, relPath, opts)` harvests inline code spans matching
`ident ( "::" ident )* "()"?` whole-span, applies six exclusions, reduces each
survivor to a needle, and resolves distinct needles through
`SymbolQuery::findDefinition`. It **reports and never judges** — see INV-4 in
`docs/specs/ANTS-3661.md`.

- **INV-1 candidate harvest** — a span is a candidate only with all six
  exclusions applied: paths (excluded by the production itself), `doc-examples`
  regions, the short-lowercase floor (`minIdentChars`, waived for a `::`-,
  `()`- or mixed-case span; a longer bare lowercase word clears it here and is
  filtered later at emission instead — INV-9), language keywords, and the two
  injected halves
  (verb names, schema arguments/refusal codes) supplied via
  `Options::excludedNames`. Fixture: `parseSpecBody`, `Foo::bar()`,
  `src/a.cpp`, `ok`, `return`, `spec_query`, `caller_cwd`, a fenced `Widget`
  and a `Ghost` inside a `doc-examples` region → exactly the first two survive.
- **INV-2 needle after last scope** — `Foo::bar` resolves on `bar`; `symbol`
  echoes the span verbatim. Reporting the needle would make the finding
  un-greppable against the doc.
- **INV-3 every unambiguous candidate reported** — resolved or not, each
  occurrence is a `symbols[]` row; only unresolved ones produce a `DocFinding`,
  and never with `autoFixable`. Both fixture rows are mixed-case so this row and
  INV-9 stay independently falsifiable.
- **INV-9 ambiguous spans reported only when resolved (ANTS-3692)** — a bare
  lowercase span (no `::`, no `()`, no case boundary) reaches `symbols[]` only
  if it resolves; otherwise it is dropped from `symbols[]` and `findings[]`.
  Fixture: `render_frame` (resolves, and is bare lowercase *on purpose* —
  shell/Python/Lua name functions that way), `check_stats` (does not resolve),
  `AlsoMissing` (does not resolve, mixed case) → `symbols[]` is the first and
  third, one finding, naming `AlsoMissing`. The third row is what stops the
  rule being satisfied by suppressing every unresolved candidate.
- **INV-10 ambiguous needles resolve last; `truncated` survives the drop
  (ANTS-3692)** — with `maxSymbolsPerRun` at 1 and the ambiguous span *first*
  in document order, the budget must still go to the unambiguous needle, and
  `truncated` must be true even though `counts.not_checked` is 0. Deriving
  `truncated` from `notChecked > 0` (the pre-ANTS-3692 form) reports a complete
  run here.
- **INV-5 document order** — `symbols[]` ascends by `docLine` then `docCol`,
  stable across runs.
- **INV-7 elided needle is `not_checked`** — a needle the run never looked up
  (`maxSymbolsPerRun` or the resolve deadline) is `not_checked`, never
  `unresolved`, and emits no finding; `truncated` is set. The uncapped half of
  the test is what makes the capped half falsifiable: without it, an engine
  that reports everything `not_checked` passes.

INV-4 and INV-6 are verb-layer rows and live in
`tests/features/doc_symbols_verb/`.

## Verification notes

Resolution runs against a seeded temp source tree rather than a mock: the
contract is "`SymbolQuery`'s ladder, unchanged", and a mock would let the two
drift without a test noticing.

### Verified RED by mutation

A compile failure would have proved nothing here: most of INV-1's rows are
*exclusions*, and every one of them passes against an engine that harvests
nothing at all — which is exactly the state before the implementation landed.
So each exclusion was removed in turn and the row had to turn red. Recorded as
run, including the one that did not.

| Mutation | Row | Result |
|---|---|---|
| M1 widen the production to admit `/` and `.` | INV-1 | **GREEN — survived** (see below) |
| M1b widen the production **and** bypass `isValidSymbol` | INV-1 | RED — harvests `src/a.cpp` |
| M2 drop the `doc-examples` region check | INV-1 | RED |
| M3 drop the `minIdentChars` floor | INV-1 | RED |
| M4 drop the keyword list | INV-1 | RED |
| M5 empty `excludedNames` (verb names + schema args together) | INV-1 | RED — two rows at once |
| M8 always report `not_checked` | INV-7 | RED |
| M10 the shipped pre-ANTS-3692 engine (emits every candidate) | INV-9 | RED |
| M11 the shipped pre-ANTS-3692 engine (document-order budget) | INV-10 | RED |

**M10 and M11 were not hypothetical — they were the shipped state**, run before
the engine changed. INV-9 failed on `r.total` 3 against 2 and
`r.findings.size()` 2 against 1 (`check_stats` emitted with a finding); INV-10
failed on `r.symbols.size()` 2 against 1 (the ambiguous span took the single
unit of budget and `AlsoMissing` came back `not_checked`). Both green after.

**M1 surviving is a property, not a hole.** The path exclusion is enforced
twice and independently: the production rejects `src/a.cpp`, and even if it did
not, the needle reduction leaves a string containing `/` and `.` that
`SymbolQuery::isValidSymbol` refuses. One mutation cannot redden a row two
guards defend. M1b removes both and the row turns red with `src/a.cpp` in the
harvest, which is what proves the row is not vacuous — the fact M1 alone was
meant to establish.

INV-4 and INV-6's absence-asserting mutations (M6, M7) are recorded in
`tests/features/doc_symbols_verb/spec.md`.

### Corpus calibration

`DocSymbols.DISABLED_CorpusCalibration` sweeps the live `docs/specs` +
`docs/standards` tree and prints the measured split against
`docs/specs/ANTS-3661.md`'s two numeric gates. It is `DISABLED_` because it
measures the repository rather than asserting a contract, and its numbers move
as the corpus grows. Re-run it when either gate is in question:

```
./build/test_claude --gtest_also_run_disabled_tests \
    --gtest_filter=DocSymbols.DISABLED_CorpusCalibration
```

The measured numbers and what they changed are recorded in the spec's § 2.1
and § 4.

## Out of scope

- **Deciding defect vs forward reference** — the whole design constraint
  (`docs/specs/ANTS-3661.md` § 2.3). A permanent exclusion, not deferred work.
- **Signature checking** — "does `foo()` still take two arguments" needs a
  parse, not a lookup. Permanent for v1.
- **Schema argument names as an injected exclusion** — no accessor returns the
  `tools/list` payload yet. Deferred: **ANTS-3679**.
- **C++ data members / enumerators resolving at all** — `SymbolQuery`'s C++
  ladder has no data-member pattern, so `autoFixable` and friends resolve
  nowhere. A resolver gap, not this engine's: **ANTS-3668**.
