# doc_lint_verb — verb-layer conformance

Contract for `tests/features/doc_lint_verb/test_doc_lint_verb.cpp`. Owning spec:
[`docs/specs/ANTS-3663.md`](../../../docs/specs/ANTS-3663.md).

Phase-1 rows only. `RemoteControl::cmdDocLint` needs a live `MainWindow`, so
behavioural rows drive the pure helper `docLintBuildResponse` — and, for the
half INV-2 asserts about narrowing, the engine — while wiring rows source-scrape
the registration sites. Same split as the doc_integrity, doc_symbols and
doc_dedup verb lanes.

## What each row locks

| Row | Invariant | Claim |
|---|---|---|
| `Inv2CheckFilterAndUnknownName` | INV-2 | `checks[]` narrows findings and `counts` together, and an unknown name is refused rather than read as "all". |
| `Inv8VerbContractMinimums` | INV-8 | `caller_cwd` is Required at both sites; a supplied path is validated before the walk; a well-formed non-existent in-root path is `ok:true` with empty arrays, **not** a refusal. |
| `Inv11CapTruncatesAfterTheSort` | INV-11 | The page is a prefix of the uncapped run, the flag is set, and `counts` still describes the whole run. |
| `Inv13EtagNeverSkipsAFix` | INV-13 | The schema carries no `etag_match`, and `isEtagSupportedTool` has no entry. |

## Why INV-2's guard run exists

The narrowing half asserts that one checker's findings are absent. That passes
against an engine producing nothing at all — the state it first ran in — so the
row opens with an unfiltered run asserting both checkers really do fire on this
fixture.

## Why INV-2's refusal arm is a scrape

`cmdDocLint` resolves the project root before it validates `checks[]`, so a
handler called without a `MainWindow` refuses `bad_path` and never reaches the
`bad_args` branch. The arm is therefore a source assertion, and it asserts the
**guard** rather than the identifier — see the mutation table below for why that
distinction had to be made.

## Why INV-13 is a phase-1 row and a registration assertion

The exclusion from `isEtagSupportedTool` is unconditional from the first
version: there is one registration entry, so "the read phase has ETag support
and the fix phase gives it up" is not something the registration can express. A
test deferred to the fix phase would leave it unasserted for the whole life of
the read-only verb — exactly the window in which a reader adds the entry back by
symmetry with the five siblings.

It cannot be behavioural. A verb outside the ETag set returns no `etag`, so
there is nothing to feed back, and an arbitrary `etag_match` would fail to match
even for a verb that is in the set.

## Must fail first — what each mutation actually turned red

| Mutation | Turned red |
|---|---|
| Compute `counts` after the cap instead of before it | `Inv11CapTruncatesAfterTheSort` |
| Add `doc_lint` to `isEtagSupportedTool` — the obvious registration | `Inv13EtagNeverSkipsAFix` |
| Delete the unknown-check guard so any name reads as "all" | `Inv2CheckFilterAndUnknownName` |

The third mutation **survived its first attempt**, and the fixture was sharpened
rather than the claim softened. The original arm asserted that the handler
mentions `checkNames()`; that string also occurs in the `accepted` list the
refusal emits, so deleting the membership test left every assertion satisfied.
The arm now requires the negated membership test itself.
