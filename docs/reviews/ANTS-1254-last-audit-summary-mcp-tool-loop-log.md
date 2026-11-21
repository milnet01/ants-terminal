# ANTS-1254 — `last_audit_summary` MCP tool — review loop log

Record for [`../specs/ANTS-1254-last-audit-summary-mcp-tool.md`](../specs/ANTS-1254-last-audit-summary-mcp-tool.md) § 11. Cold-eyes loops (audit trail).

## Cold-eyes loop log

| Loop | Verified findings → fixes applied |
|----:|-----------------------------------|
| 1 | 1 critical (`projectRoot()` doesn't exist → `resolveRootCanonical(m_main)`) + 2 high (`parse_failed` family annotation; INV-6 same-second collision wording) + 4 medium (ordinal table § 2.0; example missing severity; build matrix transitives; confidence -1 phrasing). |
| 2 | 1 high (build matrix listed wrong includers — fixed to `auditengine.cpp`/`settingsdialog.cpp`/`auditdialog.h`) + 2 medium (INV count 8→10; confidence -1 reworded). |
| 3 | 3 high (severity field source via rule lookup; severity casing UPPERCASE per `severityLabel`; `html_path` lex-max fallback for separate SARIF/HTML buttons) + 3 medium (INV-3 absent-level; runs[0..] passthrough; local-FS assumption). |
| 4 | 1 high (step-1 verify tightened to `mainwindow.cpp` registry-call count ≥ 12) + 3 medium (INV-2 vs INV-6 reconciled; `m_auditSummary` grep restricted to mutable decls; html_path fallback bounded ±60 s). |
| 5 | 2 high (§ 9 OoS row 3 contradicted § 3.1 step 3 — fixed; `Finding` struct lacked `level`/`severity` strings — split to new `AuditSummaryFinding` struct) + 3 medium (`severityLabel` is static — engine carries own copy; `severity_floor` case-sensitivity documented; "emit cached envelope" wording clarified) + 3 low (token math double-count; foreign-SARIF can't surface BLOCKER/MINOR; step-6 INV count adjusted to ≥ 5/10). |

Stopped at loop 5 (the cold-eyes default cap). Loop-5 fixes
addressed the last two implementer-blockers (struct gap + spec
contradiction); subsequent loops would target precision-level
improvements that don't gate the implementer.
