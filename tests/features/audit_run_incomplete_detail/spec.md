# Feature: `audit_run` incompleteness detail + parse-failure surface (ANTS-3585)

Test contract for the three DOOM-requested `audit_run` gaps: a caller of a
big C/C++ sweep can't tell whether a scan actually finished or was cut off at
the 60 s ceiling, and one huge modern-C++ TU cppcheck can't parse is silently
absent from the findings.

Three sub-asks (from `doom-feedback-2026-07-23`):

1. **Raise the time ceiling.** The per-tool wall-clock cap ceiling
   (`kCapPerToolMax`) and the aggregate ceiling (`kAggregateCapMs`) were 60 s /
   240 s — too low for cppcheck on a 193-file tree with an 8,900-line TU, even
   on a dedicated re-run. Raised to 300 s / 900 s so a long **async** sweep
   (which survives the transport timeout) can finish. The default per-tool cap
   (30 s) is unchanged, so ordinary runs are untouched — the high cap is opt-in.
2. **Disambiguate `incomplete_tools`.** The flat `incomplete_tools[]` names the
   offenders but not *why* — a caller couldn't tell a cut-off (timed_out) from a
   crash without scanning `by_tool[]`, which the async-poll envelope doesn't even
   carry. `incomplete_tools_detail[]` adds `{tool, status, elapsed_ms,
   truncated}` per offender (`truncated == status=="timed_out"`).
3. **Surface per-file parse failures.** cppcheck is line-based and tags every
   finding with a trailing `[id]` (default template). A parse-failure id
   (`syntaxError` / `internalError` / `internalAstError` /
   `preprocessorErrorDirective` / `cppcheckError`) means the whole TU failed to
   parse — that file got ZERO coverage and would otherwise be silently missing.
   Those files surface in top-level `parse_failures[]`.

Pure INV-1..4 exercise the `AuditRunner::internal` helpers directly
(`incompleteToolsDetail`, `parseWithSuppression` → `parseFailureFiles`,
`parseFailureFiles(byTool)`); source-anchored INV-5..6 pin the raised ceilings
and the two envelope surfaces (mirrors the `audit_run_partial_envelope` /
`audit_per_tool_timeout` house pattern for calibration + envelope facts).

## Invariants under test

- **INV-1** — `incompleteToolsDetail(byTool)` returns, for each tool whose
  `status != "ok"` sorted by name, an object `{tool, status, elapsed_ms,
  truncated}` with `truncated == (status == "timed_out")`. `ok` tools are
  excluded; an all-ok map yields an empty array.
- **INV-2** — cppcheck parse-failure extraction: for `tool == "cppcheck"`,
  `parseWithSuppression(...).parseFailureFiles` collects the file of any finding
  whose trailing `[id]` is a parse-failure id; a normal finding's file
  (`[nullPointer]`) is NOT collected; the same file failing twice appears once.
- **INV-7** (ANTS-3706) — `parseWithSuppression` records, per failing file,
  a `parseFailureReasons` entry of `"<checkId>: <first diagnostic>"`. The
  FIRST parse-failure diagnostic for a file wins (later ones are cascade
  noise); a non-parse-failure id on the same file (`unknownMacro`) does not
  claim the slot. Capped at 200 chars.

- **INV-8** (ANTS-3706) — `parseFailureDetails(byTool)` returns one
  `{file, tool, reason?}` object per (file, tool), sorted by file then tool.
  Unlike the deduped `parseFailureFiles` union, a file two tools both failed
  to parse yields two rows — the reasons differ. `reason` is omitted, not
  empty, when the tool recorded no diagnostic.

- **INV-9** (ANTS-3706) — both the sync (`mainwindow.cpp`) and async-poll
  (`claudeintegration.cpp`) providers emit `parse_failures_detail`.
  `parse_failures[]` keeps its ANTS-3585 bare-path shape, so a consumer
  already parsing it is unaffected — the same `X` / `X_detail` pairing
  ANTS-3585 established with `incomplete_tools`.

- **INV-3** — `parseFailureFiles(byTool)` unions every tool's
  `parseFailureFiles`, deduped and sorted ascending.
- **INV-4** — the extraction is cppcheck-gated: an identical `[syntaxError]`
  line parsed under a non-cppcheck tool (`clazy`) yields no `parseFailureFiles`
  (its ids live in a different namespace; only cppcheck's frontend parse
  failures mean zero coverage).
- **INV-5** — the ceilings are raised: `kCapPerToolMax` > 60 (== 300) and
  `kAggregateCapMs` > 240'000 (== 900'000). The `RunRequest` default stays 30 s.
- **INV-6** — both envelope surfaces serialise the new fields: the sync provider
  (`mainwindow.cpp`) and the async-poll done-branch (`claudeintegration.cpp`)
  both emit `incomplete_tools_detail` and `parse_failures`.

## Pre-fix check

Against pre-implementation source `AuditRunner::internal::incompleteToolsDetail`
and `parseFailureFiles` do not exist and `ParsedCounts` has no
`parseFailureFiles` member (compile error), so the pure INVs fail to build; the
source-anchored INV-5/INV-6 fail because the constants are still 60 / 240'000
and neither envelope emits the new keys. Verified before wiring the fix.

Label: `features;fast`.
