# Feature: AuditDialog "Debt Sweep" tab (ANTS-1259)

## Problem

ANTS-1113 v1 shipped the `DebtSweepEngine` (scan / fix / fold-in / triage)
plus four `debt_sweep_*` MCP tools. v2 (ANTS-1259) surfaces that engine in
the Project Audit dialog as a second tab so a user can run a debt sweep,
fix mechanical items, defer the rest into ROADMAP, or allow a false
positive — without a Claude subscription.

## Contract

The dialog gains a `QTabWidget` (Audit | Debt Sweep). The Debt Sweep tab
scans via `DebtSweepEngine::scanAll`, renders findings grouped by the four
canonical categories, and offers per-finding **Fix** (auto-fixable only) /
**Defer** / **Allow** anchors plus a **Triage with AI** button.

Testable seams on `AuditDialog` (protected; driven via a test subclass):

- `static Finding debtToAuditFinding(const DebtSweepEngine::Finding &d)` —
  adapts a debt finding to the audit `Finding` shape (rule = detectorId)
  so it can reuse the allowlist machinery.
- `bool debtFixInline(const DebtSweepEngine::Finding &f)` — delegates to
  `DebtSweepEngine::applyMechanicalFix`; true iff the file was mutated.
- `bool debtDeferToRoadmap(const QList<DebtSweepEngine::Finding> &deferred,
  const QString &heading)` — allocateIds → templateDebtSweepFoldInBlock →
  insertBlock; pre-flights the heading so a misclick doesn't burn IDs.
- `bool debtAllow(const DebtSweepEngine::Finding &f, const QString &reason)`
  — writes a project-allowlist entry (atomic) and reloads.

## Invariants under test

- **INV-D1** — `debtToAuditFinding` maps detectorId→checkId, plus file /
  line / message verbatim.
- **INV-D2** — `debtFixInline` on an `orphan_q_unused` finding deletes the
  marker line (one fewer line; marker gone).
- **INV-D3** — `debtDeferToRoadmap(K findings, heading)` advances
  `.roadmap-counter` by K and inserts a
  `### 🧹 Debt-sweep fold-in (DATE)` block (carrying the K `[ANTS-N]`
  ids) after the heading. A wrong heading returns false and leaves both
  files untouched.
- **INV-D4** — `debtAllow(f, reason)` writes a matching
  `.audit_allowlist.json` entry; after the implicit reload,
  `allowlisted(debtToAuditFinding(f))` is true.

## Test notes

GUI bundle (`test_dialogs`) — constructs `AuditDialog` against a
`QTemporaryDir` project. `debtToAuditFinding` is static. No network. Label
`features;fast`.
