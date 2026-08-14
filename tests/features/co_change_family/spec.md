# co_change_family — feature-conformance contract

**Owner spec:** [`docs/specs/ANTS-3368-co-change-family.md`](../../../docs/specs/ANTS-3368-co-change-family.md).
That document is the contract; this file records which of its invariants the
test in this directory locks, so a reader can tell coverage from intent.

The verb answers one question: *given an exemplar settings field, where is
every site I must touch to mirror it?* — including the JSON string key and the
affixed derived names (`setX`, `m_X`, `XChanged`) that a whole-word symbol
search cannot reach.

## Cases

One per live invariant. INV-8 is withdrawn and has no case.

| Case | Invariant | Locks |
|---|---|---|
| `SplitWordsFormsAgree` | INV-1 | every spelling of one field reduces to the same word sequence |
| `ScanPatternWidensWithMinRun` | INV-2 | `min_run` widens the **scan**, not merely the filter |
| `MinRunIsPerStemAndClamps` | INV-3 | per-stem default and clamp; the run is contiguous in **both** sequences |
| `StopwordOnlyRunsDropped` | INV-4 | an all-stopword run carries no signal |
| `RoleVocabularyIsClosed` | INV-5 | six roles, that precedence, no seventh |
| `OrderingIsDeterministic` | INV-6 | one row per `(path, line)`; file order by max `run_len` |
| `PartialAnswersAreFlagged` | INV-7 | a capped answer says so and keeps the strongest sites |
| `RefusalCodes` | INV-9 | stem charset gate; the handler emits `bad_args` / `rg_failed` |
| `SeamTuHasNoChromeSymbols` | INV-10 | the seam TU stays pure, so it links into `test_core` alone |
| `RegistrationAndSchema` | INV-11 | registration, contract table, schema opt-ins |
| `StemCannotInjectPattern` | INV-12 | a stem cannot inject regex syntax into the `rg` argv |
| `MatchWidensToCandidate` | INV-13 | a match widens to the candidate the filter reads |
| `ScanIgnoresDeclaredSourceRoots` | INV-14 | the scan is repo-wide, not root-scoped |

## Why two of these are source-greps, not behaviour

`SeamTuHasNoChromeSymbols` and `ScanIgnoresDeclaredSourceRoots` assert
**absences** — that the seam names no chrome symbol, and that the handler does
not narrow the walk. An absence has no runtime observation; the grep is the
only surface. Both strip comments first, so prose explaining the rule cannot
be read as breaking it.

INV-10 deliberately does **not** claim the `test_core` link as a second
surface: this bundle is `test_claude`, so no `test_core` object references the
seam and that link would pass whatever the TU contained. The owner spec § 3
records the reasoning.

## Build

Compiled into the **`test_claude`** bundle — not a standalone target. Label
`features;fast`. Run with `ctest --test-dir build -R CoChangeFamily`; check
`ctest -N -R CoChangeFamily` lists 13 before trusting a green run.
