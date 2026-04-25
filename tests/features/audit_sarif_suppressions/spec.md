# Feature: SARIF v2.1.0 `result.suppressions[]` array

## Contract

The SARIF export (`AuditDialog::exportSarif`) MUST surface
user-suppressed findings (entries in `m_suppressedKeys`) as
`result` objects with a populated `result.suppressions[]` array per
the SARIF v2.1.0 specification §3.34.

Per spec, each suppression object MUST include:
- `kind`: one of `"inSource"`, `"external"` — for `m_suppressedKeys`
  (read from `~/.audit_suppress` JSONL), the value MUST be
  `"external"` because the suppression is recorded outside the
  source artifact.
- `state`: one of `"accepted"`, `"underReview"`, `"rejected"` —
  current implementation always emits `"accepted"` because the
  audit dialog has no review-workflow concept.
- `justification` (optional but emitted when available): the
  free-text reason the user typed when suppressing.

The pre-0.7.29 SARIF export silently dropped suppressed findings,
which is technically valid SARIF (a "filtered" view) but loses
auditing data: GitHub Code Scanning, SonarQube, and the VS Code
SARIF Viewer all expect to see suppressed findings with the
suppressions array so consumers can compute fires-vs-suppressions
ratios and identify noisy rules. Surfacing the suppressions array
preserves the full audit picture across export boundaries.

## Rationale

SARIF v2.1.0 §3.34 (`result.suppressions`) was added to the spec
specifically so suppression bookkeeping survives export to consumers
that do their own surfacing. By dropping suppressed findings, our
export was producing a falsely-clean report, which:
- Defeats the suppression-trend telemetry the dashboard already
  computes (`s.suppressions30d` per rule).
- Hides noise from external readers who can't cross-reference the
  local `~/.audit_suppress` file.
- Deviates from the SARIF spec's intent — suppressed findings still
  exist; they're just classified differently.

The fix needs:
1. A persistent `m_suppressionReasons: QHash<QString,QString>` keyed
   by dedup-key so the export can surface the user's `reason` field.
2. Loader (`loadSuppressions`) populates the map alongside the keys.
3. Save path (`saveSuppression`) updates the map on insert.
4. SARIF export iterates ALL findings (no `m_suppressedKeys.contains`
   filter), and for each suppressed one appends the
   `suppressions[]` array with kind/state/justification.

## Invariants

**INV-1 — `m_suppressionReasons` map declared in the header.**
Source-grep against `src/auditdialog.h`: a declaration matching
`QHash<QString, QString>\s+m_suppressionReasons` (or
`QMap<QString, QString>` — either is acceptable) must appear in the
private section of `class AuditDialog`.

**INV-2 — `loadSuppressions` populates the reasons map.** Source-grep
inside the `loadSuppressions` function body in
`src/auditdialog.cpp`: the body must contain at least one
`m_suppressionReasons` token (insert / write / clear) AND the
`m_suppressionReasons.clear()` call must be near the top alongside
`m_suppressedKeys.clear()`.

**INV-3 — `saveSuppression` updates the reasons map.** Source-grep
inside the `saveSuppression` function body: the body must contain at
least one assignment / insert into `m_suppressionReasons`.

**INV-4 — SARIF export emits a `suppressions` array.** Source-grep
inside the `exportSarif` function body: the body must contain the
literal string `"suppressions"` (a JSON property name being inserted
into a `QJsonObject`). Furthermore, the body must contain the SARIF
suppression-state literals `"external"` AND (`"accepted"` OR
`"state"`).

**INV-5 — SARIF export NO LONGER unconditionally skips suppressed
findings.** The exportSarif body MUST iterate findings without an
early-`continue` solely on the basis of
`m_suppressedKeys.contains(f.dedupKey)` or `isSuppressed(f.dedupKey)`.
We test by ensuring the body does NOT contain `continue` on a line
where `dedupKey` and `Suppressed`/`suppress` co-occur. The
`if (cr.warning || cr.findings.isEmpty()) continue;` pre-filter on
warnings is allowed and unaffected.

## Scope

### In scope
- Map declaration + load/save mirror.
- SARIF export emits suppressions[] + iterates suppressed findings.
- `kind: external`, `state: accepted` semantics.

### Out of scope
- Surfacing inline-source markers (NOLINT, noqa, nosec) as
  `kind: inSource` SARIF suppressions. Those findings are dropped at
  parseFindings/applyFilter time — they never enter
  `m_completedResults` — and reconstructing them for SARIF would
  need a second pass. Leave for a future bundle.
- Allowlist (`.audit_allowlist.json`) entries — same reason: dropped
  pre-pipeline.
- HTML and plain-text reports: the in-app UI hides suppressed
  findings by design, so the report exports retain that filter.
  Only SARIF, which targets external CI/security consumers, surfaces
  them.

## Regression history

- **Pre-0.7.29:** `exportSarif` skipped suppressed findings via
  `m_suppressedKeys.contains(f.dedupKey)` (silently filtered).
- **0.7.29 (this fix):** suppressed findings emitted with
  `result.suppressions[]` per SARIF v2.1.0 §3.34; the
  `m_suppressionReasons` map carries the user's free-text
  justification through to the export.
