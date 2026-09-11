# mcp_likely_fix — `likely_fix` add_include hint for undeclared-symbol diagnostics

**Status:** shipped
**Kind:** feature
**Source:** cc-feedback-2026-06-30 (Vestige Sug-C) — ANTS-3374

## Problem

`recent_errors` and `build_status` surface compiler diagnostics but stop
there. The single most common C++ build error — an undeclared symbol from a
missing `#include` (`'DeviceHotSwapMode' has not been declared`) — leaves the
Claude session to run a second `find_definition(X)` verb by hand to learn which
header to add. That two-verb diagnose→fix loop is stitchable: on such a
diagnostic, resolve the symbol's declaring header and attach it inline.

`recent_errors` is `TabSpecific`, which keeps it on the GUI thread. A failed
build commonly produces many undeclared-symbol diagnostics from one missing
include, and the original enrichment resolved each distinct symbol with its
own tree walk — one `SymbolQuery::findDefinition` call per symbol, up to the
existing lookup cap. `symbolquery.cpp`'s own measurement puts one walk at
roughly 81 ms on this tree, so a bursty failed build could cost seconds on
the GUI thread right after the build that produced the burst. ANTS-5053 is
the fix: resolve the whole burst in the one walk `SymbolQuery::findDefinitions`
already does for exactly this shape of call, applying `resolveHeader`'s
selection rule to each result instead of re-walking per symbol.

## Surface

New pure helper `BuildFixHint` (`src/buildfixhint.{h,cpp}`, `ants_core_lib`):

- `undeclaredSymbol(message)` → the undeclared identifier named by a
  recognised diagnostic, or `""`.
- `resolveHeader(rootCanonical, symbol)` → the project-relative header that
  declares `symbol` (reusing `SymbolQuery::findDefinition`), or `""`.
- `resolveHeaders(rootCanonical, symbols)` → a symbol-keyed map, each value
  equal to `resolveHeader(rootCanonical, thatSymbol)`, computed with one
  `SymbolQuery::findDefinitions` walk instead of one walk per symbol.

Both `cmdRecentErrors` and `cmdBuildStatus` (read path) post-process their
`errors[]` array: for each entry whose `message` names an undeclared symbol
that resolves to a project header, attach

```json
"likely_fix": { "add_include": "src/foo.h", "defines": "X", "at": "src/bar.cpp" }
```

`add_include` is the repo-relative header path (consistent with
`find_definition` file reporting). `at` is the failing file (the entry's
`file`), omitted when empty. Enrichment is best-effort: an unresolved root or
symbol simply yields no `likely_fix`, never a verb-level failure.

## Invariants

- **INV-1** — `undeclaredSymbol` extracts the identifier from all four
  recognised forms: GCC `'X' has not been declared`, GCC `'X' was not declared
  in this scope`, clang `unknown type name 'X'`, clang `use of undeclared
  identifier 'X'`. An unrelated message (e.g. `redefinition of 'X'`) yields
  `""`.
- **INV-2** — `resolveHeader` returns the header-suffixed match when the symbol
  is declared in a `.h`/`.hpp`/… file, in preference to any source-file match.
- **INV-3** — When the symbol is defined only in a source file (`foo.cpp`),
  `resolveHeader` falls back to the on-disk sibling header (`foo.h`) and returns
  `""` when no such sibling exists.
- **INV-4** — Self-gating: a symbol that resolves nowhere in the project yields
  `""` (no spurious suggestion for a typo'd local). An empty root or invalid
  symbol also yields `""`.
- **INV-5** — Wiring: both `cmdRecentErrors` and `cmdBuildStatus` reference the
  `BuildFixHint` enrichment on their diagnostics array, deduplicating repeated
  symbols and bounding the number of distinct `resolveHeader` lookups.
- **INV-6** — Batch equivalence: `resolveHeaders(root, symbols)` returns, for
  every distinct symbol in `symbols`, exactly the value `resolveHeader(root,
  thatSymbol)` returns — including a duplicate symbol and an invalid one, and
  including the all-empty result an empty root produces. This holds whatever
  `resolveHeaders` does internally; it is the contract callers rely on, not an
  implementation detail.
- **INV-7** — Batch walks once: `resolveHeaders`'s body calls
  `SymbolQuery::findDefinitions` (the one-walk batch query) and does not call
  `resolveHeader` or the singular `SymbolQuery::findDefinition` per symbol.
  `enrichLikelyFixes` calls `BuildFixHint::resolveHeaders` exactly once per
  invocation and no longer calls `BuildFixHint::resolveHeader` at all — a
  diagnostics burst no longer costs one tree walk per undeclared symbol.

## Tests

`test_mcp_likely_fix.cpp` (in the `test_claude` bundle):

1. `undeclaredSymbol` across the four positive forms + a negative (INV-1).
2. `resolveHeader` against a seeded `QTemporaryDir` project: header match
   (INV-2), source-only sibling fallback + missing-sibling → `""` (INV-3),
   unresolved symbol / empty root → `""` (INV-4).
3. Source-grep of `remotecontrol.cpp` asserting both verbs invoke the
   enrichment (INV-5).
4. `resolveHeaders` against the same seeded project, called with every symbol
   the `resolveHeader` test already checks plus a duplicate and an invalid
   symbol, compared value-for-value against `resolveHeader`; plus an
   empty-root call (INV-6).
5. Source-grep of `buildfixhint.cpp`'s `resolveHeaders` body for the batch
   walk and the absence of a per-symbol walk (INV-7).
6. Source-grep of `enrichLikelyFixes`'s body for exactly one `resolveHeaders`
   call and no remaining `resolveHeader` call (INV-7).

Must-fail-first (ANTS-3374, original ship): the helper is absent pre-fix, so
the test fails to compile against feature-absent code; it passes once
`buildfixhint.{h,cpp}` and the two wiring points land.

Must-fail-first (ANTS-5053, this extension): INV-6 is a behavioural guard and
passes against both the per-symbol stub and the batched fix — it exists to
catch a batching bug, not to distinguish the two. INV-7 is source-only and
fails against the stub, because the stub still walks once per symbol inside
`resolveHeaders` and `enrichLikelyFixes` still calls the singular
`resolveHeader`; it passes once `resolveHeaders` is rewritten around
`SymbolQuery::findDefinitions` and `enrichLikelyFixes` is rewired to call it
once.
