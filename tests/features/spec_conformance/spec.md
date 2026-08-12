# Feature: spec_conformance — run a spec's patterns against its examples

Test contract for ANTS-4108
(`docs/specs/ANTS-4108-spec-conformance-verb.md`). Locks the behavioural
invariants of `SpecConformance::run`, the pure engine behind the
`spec_conformance` MCP verb.

**The parent spec is accepted-with-caveat**: its gate reached the 3-loop cap
without a clean pass, and § 2.3–§ 2.6 (the envelope and extraction taxonomy)
are marked provisional. **These fixtures are therefore the arbiter.** Where a
fixture and the prose disagree, the fixture wins and the spec is amended
(`/write-spec` Step 8 — fold back what implementation found), never the other
way round.

The engine is a free function taking `(absPath, Options)` and returning the
§ 2.3 envelope, so the test drives it directly against temp fixture files —
no QTemporaryDir gymnastics beyond writing a `.md`, no GUI, no MainWindow.

## Invariants under test (mirrors ANTS-4108 § 3)

- **INV-1** — a fence declaring an engine, followed by an `input`/`expected`
  table, yields one case per row. Only TOP-LEVEL fences are scanned: a
  `regex` fence nested inside a longer-delimiter fence yields nothing, which
  is what stops the verb extracting cases from a spec's own illustration.
- **INV-2** — a row whose actual result differs from `expected` is a FINDING
  carrying pattern, input, expected and actual. The reporter's own case:
  `\d{1,5}(?![0-9])` against `" 123456"` expecting `no match` yields `23456`.
- **INV-3** — the three outcomes stay distinct: `no match`, `no capture` (a
  group that did not participate), and an empty backtick pair (a group that
  captured the empty string). Discriminated by
  `QRegularExpressionMatch::hasCaptured`, never by string comparison —
  `QString() == QString("")` is true in Qt.
- **INV-4** — a fence with a declared engine and no table is a CANDIDATE
  (`pattern_without_expectation`), never a finding and never dropped. A bare
  `regex` fence is `engine_not_declared`, and a fence yields at most one
  candidate.
- **INV-5** — an unrecognised engine is refused `unsupported_engine`, per
  case, and never run under a substitute. A table-less `regex python` fence
  emits one refusal, `cases_run == 0` and `candidates == []`; the same fence
  beside two valid cases still runs those two.
- **INV-6** — the verb writes nothing.
- **INV-7** — the caps are enforced and reported: an over-cap pattern or
  input yields a `too_large` refusal; more cases than `max_cases` sets
  `truncated: true`; a `max_cases` outside `[1, 1000]` refuses the call.
- **INV-9** — two runs over an unchanged file agree except for
  `observations`, and the etag is stable across them.

INV-8 (no subprocess, no interpreter) is a source-scrape over
`src/specconformance.{h,cpp}`, not a runtime case — no running test can
observe the absence of an interpreter.

## Pre-fix check

Against the stub engine (`run()` returning an empty envelope) every
behavioural assertion fails on its assertion, not on a link error — the
stub exists precisely so the red run proves the fixtures exercise something.

Label: `features;fast`.
