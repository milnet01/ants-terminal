# Feature: audit-tool self-learning — RuleQualityTracker invariants

## Contract

`RuleQualityTracker` (`src/auditrulequality.h`) is the data layer behind
the 0.6.31 audit dialog's **Rule Quality** view. It records per-rule
fire and suppression events to `<project>/audit_rule_quality.json` and
exposes:

1. **`report()`** — per-rule stats covering the last 30 days, sorted by
   suppression rate (highest first). For each rule:
   - `fires30d`, `suppressions30d`, `firesAllTime`, `suppressionsAllTime`
   - `lastFire`, `lastSuppression` timestamps
   - `fpRate30d` = `100 * suppressions30d / fires30d`, capped at 100;
     `-1` when `fires30d == 0` (no division-by-zero).

2. **`suggestTightening(ruleId, maxSamples=5, minLength=6)`** — heuristic
   tightening proposal. Computes the longest common substring across the
   last `maxSamples` suppressed line texts for `ruleId`. Returns:
   - empty string when fewer than 2 samples exist for the rule
   - empty string when the LCS is shorter than `minLength`
   - empty string when the LCS contains no structural boundary
     character (space / paren / brace / bracket / semicolon / quote /
     angle bracket) — pure-identifier substrings tend to suggest
     project-noun-specific filters rather than rule-shape filters
   - the LCS otherwise

3. **Persistence round-trip** — `save()` then `reload()` must reproduce
   the same `report()` output. Schema is `audit_rule_quality.json` v1
   with `fires` and `suppressions` arrays of `{rule, line, ts, …}`
   objects.

4. **Retention** — records older than 90 days are pruned on `save()`.
   Beyond `MAX_RECORDS = 50000` per category, the oldest entries are
   tail-clamped (FIFO).

## Rationale

The audit catalogue's signal-to-noise ratio drifts as the codebase
evolves. The 2026-04-16 triage caught 137 of 141 findings as
false positives; that round of tightening was a one-shot manual pass.
Without instrumentation the same drift will silently re-accrue —
shipped rules don't get worse on their own, but the *codebase* changes
in ways that turn previously-clean rules noisy.

`RuleQualityTracker` makes that drift observable per-rule so the user
can act on the worst offenders first, and the LCS suggester turns the
last few suppressions for a noisy rule into a one-line proposed
tightening that's just a copy-paste away from the user's
`audit_rules.json` override pack.

The deeper version of this loop (LLM-driven cross-rule analysis of
suppression patterns) lives in the weekly cloud routine documented in
`docs/RECOMMENDED_ROUTINES.md` §3 — that's a much heavier pass that
would be wasteful to run on every dialog open. The in-process tracker
is the lightweight always-on layer.

## Scope

### In scope
- Fire and suppression recording APIs.
- 30-day-window aggregation in `report()`.
- LCS suggester returns matching long substring with structural boundary.
- LCS suggester returns empty for too-few samples / too-short LCS /
  pure-identifier LCS.
- JSON persistence round-trip.

### Out of scope
- The Rule Quality dialog UI itself (Qt rendering is hand-tested).
- Hooking the tracker into `AuditDialog::onCheckFinished` and
  `AuditDialog::saveSuppression` (covered by the AuditDialog being
  exercised by the existing `audit_rule_fixtures` test, plus manual
  smoke).
- Deep ML-driven tightening proposals — those are explicitly the
  cloud routine's job.
- Concurrency / multiple processes writing the same file at the same
  time. Single-process Qt app, single-dialog scope; the JSON write is
  truncate-then-write so a crash mid-write loses the last delta but
  doesn't corrupt the file.

## Test strategy

Headless test that constructs `RuleQualityTracker` against a
`QTemporaryDir`, drives recordFire / recordSuppression, then asserts
the report and suggester behaviour. Round-trips through save() →
reload() with an integrity check on the report shape.

No QApplication needed — the tracker is pure model code. Same approach
as the OSC 133 HMAC test.
