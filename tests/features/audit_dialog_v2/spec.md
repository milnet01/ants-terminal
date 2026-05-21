# Feature: AuditDialog v2 UI affordances (ANTS-1257)

## Problem

ANTS-1111 v1 (0.7.88) shipped the engine layer — `RoadmapFoldIn`,
`AuditEngine::templateRoadmapFoldInBlock`, `AuditHygiene::semgrepRulePacks`
— but deferred the AuditDialog UI that drives them. ANTS-1257 (v2) wires
four affordances onto the existing dialog:

1. **"Fold actionable into ROADMAP"** footer button — orchestrates
   `allocateIds` → `templateRoadmapFoldInBlock` → `insertBlock`.
2. **"Allow this finding"** per-finding link — appends a matching entry to
   `.audit_allowlist.json` so future runs drop the finding.
3. **"Since baseline"** filter pill — shows only new findings on
   recently-changed lines.
4. Wires `AuditHygiene::semgrepRulePacks(detectProjectFrameworks(...))`
   into the semgrep invocation (not separately tested here; verified by the
   v1 `audit_framework_detect` test on the helper).

## Contract

Testable seams on `AuditDialog` (protected; driven via a test subclass):

- `bool appendAllowlistEntry(const Finding &f, const QString &reason)` —
  atomic (`QSaveFile`) append of `{rule, path_glob, line_regex, reason}`
  matching `f`, then reload.
- `bool foldFindingsIntoRoadmap(const QList<Finding> &actionable,
  const QString &releaseHeading)` — single-shot orchestration; false (no
  write) on empty set / id-alloc failure / heading-not-found.
- `static bool visibleSinceBaseline(const Finding &f, recentLines,
  baselineFingerprints)` — the "Since baseline" predicate.
- `QList<Finding> actionableFindings() const` — visible findings with
  `aiVerdict == TRUE_POSITIVE` OR `confidence >= 70`.

## Invariants under test

- **INV-11** — `visibleSinceBaseline` keeps a finding iff its `(file,line)`
  is in `recentLines` AND its `dedupKey` is NOT in `baselineFingerprints`.
  Unfiled findings (no file/line) pass the recent test; a finding whose
  file isn't in the changed set is dropped.
- **INV-13** — `appendAllowlistEntry(f, reason)` writes exactly one entry
  whose `(rule, path_glob, line_regex)` triple matches `f`; after reload
  `allowlisted(f)` is true and an unrelated finding stays false. The write
  is atomic.
- **INV-14** — `foldFindingsIntoRoadmap(K findings, heading)` advances
  `.roadmap-counter` by exactly K and inserts a
  `### 🔍 Audit fold-in (DATE)` block (carrying the K allocated
  `[ANTS-N]` ids) after the named heading. A wrong heading returns false
  and leaves both files untouched.

## Test notes

GUI bundle (`test_dialogs`) — constructs `AuditDialog` against a
`QTemporaryDir` project. `visibleSinceBaseline` is pure/static and driven
without an instance. No network. Label `features;fast`.
