# cited_by — feature contract

Test contract for **ANTS-3716** (`docs/specs/ANTS-3716-cited-by-sweep.md`).
`cited_by` answers one question in one call: given the anchors a change touched,
which documents cite which of them.

Cases run **behaviourally, against real fixture trees and the real ripgrep**,
because every invariant here is about what rg actually does — occurrences versus
lines, event order across files at `--threads 4`, exit 2 on a path that is not
there, a match event per positional path that reaches a file. A stubbed rg would
assert the behaviour the author already believed, which is the failure mode the
spec's rejected single-pass design was caught by.

Each case builds its own `QTemporaryDir` project. The root is **canonicalised**
before use: `/tmp` is a symlink on some hosts, and the handler trims rg's
absolute paths against the canonical root, so an uncanonicalised root leaves
every `file` absolute and fails every cell assertion for the wrong reason.

## Cases

| Case | Invariant | Asserts |
|---|---|---|
| `Inv1CellPerAnchorFilePairCountsOccurrences` | INV-1 | One cell per (anchor, file) pair, `count` = **occurrences**. A fixture citing two anchors on one line, one anchor on two lines, and one anchor **twice on a single line** → 4 cells with counts 1, 1, 2, 2. `first_line` is the pair's lowest line. |
| `Inv2MatchedUnmatchedPartitionSurvivesTruncation` | INV-2 | The two arrays partition `anchors` in request order; then, with `max_cells:1` over a fixture whose second anchor's only cell sorts past the cap, that anchor is **still** in `anchors_matched`. |
| `Inv3LiteralAndUnescaped` | INV-3 | Anchors reach rg unescaped under `--fixed-strings`: `foo.cpp` matches `foo.cpp` and not `fooXcpp`, and an anchor containing `é` returns `ok:true`. |
| `Inv4CaseModesAndNoSmart` | INV-4 | `insensitive` is the default, `sensitive` narrows, `smart` refuses `bad_args`. |
| `Inv5DefaultScope` | INV-5 | An omitted `scope` resolves to the docs dir, `README.md` and `CLAUDE.md` — a match under `other/` does not appear. |
| `Inv6ScopeEscapeAndAnchorArityRefuse` | INV-6 | `scope:["../outside"]` → `bad_path`; `anchors:[]` → `bad_args`; paired with a positive control over the same fixture. |
| `Inv7SortedBeforeCapAndStable` | INV-7 | `cells` is sorted by (anchor, file) and the cap runs after the sort: `cells[0].anchor == "alpha"` on a **single-file** fixture where `zeta` is cited on an earlier line, two files under one anchor in path order, and two truncated calls returning byte-identical envelopes. |
| `Inv8CappedCellsUncappedFilesCount` | INV-8 | 5 cells over **4 distinct files** with `max_cells:2` → `cells.size()==2`, `cells_count==2`, `truncated:true`, **`files_count==4`**. |
| `Inv9OneRgCallSiteAndNoProcessInHandlers` | INV-9 | Exactly one `rg.start(` across the RemoteControl TUs; neither handler's body names `QProcess`; the `RgRun` return type carries no refusal envelope. |
| `Inv10FailedRunRefusesWithNoPartialGuard` | INV-10 | `cmdCitedBy` carries **no** `matches.isEmpty()` guard on a failure branch and its `rg_failed` message names this verb. Comments are stripped first — the handler's own comment names the guard it deliberately drops. |
| `Inv11MissingScopeEntriesArePruned` | INV-11 | A project with no `README.md` / `CLAUDE.md` returns `ok:true` on the **default** call with `scope_resolved:["docs"]`; `scope:["nope"]` → `ok:true`, `scope_resolved:[]`, every anchor unmatched. |
| `Inv12OverlappingScopeIsDeOverlapped` | INV-12 | `scope:["docs","docs/sub"]` over one matching file under `docs/sub/` → one cell with `count` 1, not 2, and `scope_resolved:["docs"]`. |
| `Inv13EmptyAnchorRefuses` | INV-13 | `anchors:["", "oldName"]` → `bad_args`; paired with the positive control `anchors:["oldName"]` → `ok:true`. |

## Must-fail-first — run, not asserted

The verb is new, so each behavioural case was proved against the **shipped**
implementation with the rule under test mutated out. Applied in two disjoint
batches, built, observed, then reverted (2026-08-14):

| Case | Mutation | Observed |
|---|---|---|
| INV-1 | occurrence tally replaced by a flat `1` per match event | the twice-on-one-line anchor came back `count` 1 — the per-line reading |
| INV-8 | `files_count` derived from the capped `cells` array | 2 instead of 4: the cap read as completeness |
| INV-13 | the empty-anchor refusal removed | `rg -F -e ''` ran and the call returned `ok:true` |
| INV-3 | `QRegularExpression::escape` applied to the anchor | `foo.cpp` stopped matching itself and the accented anchor found nothing |
| INV-7 | the emitted cell order reversed after the sort | `cells[0].anchor` came back `zeta`; INV-1's ordering assertions went red with it, as collateral |
| INV-12 | the scope de-overlap dropped | `count` doubled to 2 and `scope_resolved` echoed both entries |

INV-9 and INV-10 are source scrapes and cannot fail before the code exists:
INV-9 asserts a refit, and INV-10's trigger is unprovokable from a committable
fixture — `timeout_sec` floors at 1 s and rg searches hundreds of MB in well
under that, so a live trigger would need a several-hundred-MB fixture in a
`features;fast` bundle and would still be load-dependent on a 4-vCPU CI host.
That is a flake, not a test. Same precedent as
`tests/features/mcp_workspace_search_timeout_sec/` INV-3 / INV-4.

## Would break this

- Tallying matching **lines** instead of occurrences → INV-1.
- Deriving `files_count` from the capped array → INV-8.
- Truncating during a run rather than after the sort → INV-7, and INV-2's
  matched anchor would fall into `anchors_unmatched`.
- Escaping the anchor → INV-3; the accented case exits 2 with a regex parse
  error, which INV-10 would then turn into a refusal for the whole call.
- Passing a missing scope entry to rg → INV-11: the **default** call refuses on
  any project without a `README.md`.
- Searching overlapping scope entries → INV-12's doubled `count`.
- Offering `case:"smart"` → INV-4; rg resolves it over the combined pattern set,
  so one anchor would change every other anchor's result.
- Adding a per-run measurement (`elapsed_ms`) to the envelope → the central
  ETag hashes the whole response, so the 304 could never fire.
