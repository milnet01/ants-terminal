# Audit-engine extraction (ANTS-1119 v1)

Per [ADR-0002](../../../docs/decisions/0002-cold-eyes-companion-cleanup.md)
decision 4: lift the engine + triage halves of `auditdialog.cpp` into a
new `auditengine.{h,cpp}` module that builds against `Qt6::Core` only.
v1 scope is the data-shape + three demonstrably-pure parsing functions
(`applyFilter`, `parseFindings`, `capFindings`); follow-up bullets
migrate the rest. See `docs/specs/ANTS-1119.md`.

## Surface

- `src/auditengine.h` — data types (CheckType, Severity, OutputFilter,
  Finding, CheckResult, AuditCheck) + free functions in
  `namespace AuditEngine`.
- `src/auditengine.cpp` — implementations.
- `src/auditdialog.{h,cpp}` — thin presentation layer; calls
  `AuditEngine::applyFilter / parseFindings / capFindings` directly
  from the orchestration path.

## Invariants

- **INV-1** `src/auditengine.h` exists and includes only Qt6::Core
  headers (no `QWidget`, `QPainter`, `QDialog`, `QPushButton`,
  `QTextBrowser`, `QPaintEvent`, `QStyleOption*`).
- **INV-2** `src/auditengine.cpp` exists and includes only Qt6::Core
  headers (same exclusion list).
- **INV-3** `auditengine.h` declares the five engine-side data types
  (`CheckType`, `Severity`, `OutputFilter`, `Finding`, `CheckResult`,
  plus `AuditCheck`).
- **INV-4** `auditengine.h` declares the three v1 free functions
  (`applyFilter`, `parseFindings`, `capFindings`) inside
  `namespace AuditEngine`.
- **INV-5** `auditdialog.h` includes `auditengine.h` so existing call
  sites that include `auditdialog.h` continue to see the data types
  transitively.
- **INV-6** `auditdialog.cpp` no longer defines bodies for
  `AuditDialog::applyFilter`, `AuditDialog::parseFindings`, or
  `AuditDialog::capFindings` — the engine owns them.
- **INV-7** `auditdialog.cpp` calls `AuditEngine::applyFilter`,
  `AuditEngine::parseFindings`, and `AuditEngine::capFindings`
  through the orchestration path (the three references are present).
- **INV-8** Behavioural: `AuditEngine::capFindings` against a
  synthetic `CheckResult` with N=10 findings + cap=3 trims to 3 +
  records `omittedCount=7`.
- **INV-9** Behavioural: `AuditEngine::parseFindings` against a
  synthetic `file:line:col: msg` line populates `f.file`, `f.line`,
  and produces a 24-char hex `dedupKey`.

## Out of scope (deferred to follow-up bullets)

- The full ≥30 % LOC drop ANTS-1119 spec INV-2 — v1 doesn't hit
  it because v1 only moves three functions. Future bullets move
  more. Documented in the journal at close-out.
- byte-identical SARIF/HTML INVs — these compose with the audit
  pipeline as a whole; they're locked by the existing
  `audit_rule_fixtures` shell harness, which still passes
  unchanged after the v1 extraction.
