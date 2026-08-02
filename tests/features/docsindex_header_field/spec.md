# docs_index reads Status with the shared header-field rule

**Feature:** `src/docsindex.cpp::scanDoc()` buffers the header block and calls
`SpecParse::headerField` once, instead of carrying a third copy of the
`**Status:**` extent rule with the ANTS-3672 truncation bug.
**Owner spec:** [`docs/specs/ANTS-3786-docsindex-header-field.md`](../../../docs/specs/ANTS-3786-docsindex-header-field.md).
**Label:** `features;fast`.

## Contract

| INV | Assertion | Test |
|---|---|---|
| INV-1 | A wrapped `**Status:**` yields the whole joined value, not its first physical line | `WrappedStatusIsJoinedWhole` |
| INV-2 | Within four named exclusions, `docsindex` and `headerField` return the same value — and outside them they demonstrably differ | `MatchesSharedRuleWithinBounds`, `ExcludedLinesMakeTheTwoDiverge` |
| INV-3 | A `**Status:**` only below the first `^## ` leaves `status` empty | `StatusBelowHeaderBlockIsNotRead` |
| INV-4 | The buffer is capped at `maxHeaderBlockLines`, silently | `HeaderBlockCapIsSilentAndBounded` |
| INV-5 | A budget-truncated read yields `""`, never a partial field value | `BudgetTruncatedReadYieldsEmptyStatus` |
| INV-6 | A document with no `**Status:**` is indexed with `""` and is not an error | `NoStatusLineIsNotAnError` |
| INV-7 | `docsindex.cpp` adopts the shared rule and keeps no matcher of its own | `DocsIndexAdoptsTheSharedRule` |
| INV-8 | A document with no `^## ` anywhere still yields its status (EOF flush) | `HeaderBlockIsFlushedAtEof` |
| INV-9 | `tools/spec-header-survey.py --scope=docs-index` reproduces the five classes | `SurveyToolReproducesTheClasses` |

## Fixtures

`fixtures/` holds hermetic documents authored for this test — **not** the
repo's real corpus, which would make the suite fail whenever someone edits an
unrelated spec and would re-import the owner spec's dated counts into a test
that must not depend on them. Each fixture root is copied into a
`QTemporaryDir` before `DocsIndex::build()` walks it, so a walk can never write
into the source tree.

- `corpus/` — exactly one document per simulator class, in `walkDocs` shape
  (root `*.md` plus `docs/**/*.md`). This is INV-9's tree; its counts are
  `1/1/1/1/0`, and the trailing zero is `other`.
- `eof-flush/` — a document with a header block, a `**Status:**` and no `##`
  heading anywhere (INV-8).
- `exclusions/` — one document per `scanDoc` skip, each placing the excluded
  line **between** the field and the prose after it, so the absorbed-prose
  effect is what the test observes (INV-2's difference half).

INV-4 and INV-5 are driven by `Options` overrides on inline documents rather
than by fixtures: what they pin is a bound, not a document shape.

## Must fail first

Each assertion is verified to fail against pre-fix `src/docsindex.cpp` before
the fix is restored, per the project test convention.
