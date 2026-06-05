# ANTS-2032 — audit_run surfaces partiality + SARIF-before-reply

## Background

One RetroDB `audit_run` (ruff+bandit+semgrep+gitleaks, top_findings=60)
returned "empty reply from Ants MCP" + a terminal relaunch — filed LOW
and most likely a symptom of the crash, not an audit_run fault. The
bullet asks for two defensive guarantees regardless of root cause:
(1) write the SARIF artifact to `.audit_cache/` BEFORE serialising the
inline reply, so a too-big/failed reply still leaves an artifact for
`last_audit_summary`; (2) return a partial envelope with whatever tools
completed rather than all-or-nothing empty when one tool blows a budget.

Investigation found both already structurally true: `writeSarif` runs
inside `runAudit` before it returns, and the caller serialises the
envelope only after `runAudit` returns (guarantee 1); each tool runs
under a per-tool wall-clock cap that records `timed_out`/`crashed`
status without aborting the others, and `runAudit` returns whatever
completed (guarantee 2). The missing piece was that partiality was not
surfaced explicitly — a caller had to scan `by_tool[].status`.

ANTS-2032 adds a first-class `partial` flag + `incomplete_tools[]` list.

## Invariants

### INV-1 — incompleteToolNames lists non-ok tools, sorted

`AuditRunner::internal::incompleteToolNames(byTool)` returns the names
of tools whose `status != "ok"` (timed_out / crashed), ascending. A map
of all-ok tools yields an empty list.

### INV-2 — partial ⟺ a tool did not finish ok

A run with at least one timed_out/crashed tool is partial; an all-ok
run is not. (Derivation: `partial = !incompleteTools.isEmpty()`.)

### INV-3 — envelope serialises the partial surface

The `audit_run` provider writes `partial` (always) and
`incomplete_tools` (when non-empty) onto the envelope, and the
descriptor advertises them.

### INV-4 — SARIF is written before the reply is serialised

`writeSarif` is invoked inside `runAudit` (which returns the struct the
caller then serialises) — verified by source order so the
"artifact-before-reply" guarantee can't silently regress.

## Test plan

INV-1/INV-2 are behavioural over the pure helper (synthetic ToolResult
maps). INV-3/INV-4 are source-anchored against mainwindow.cpp /
auditrunner.cpp (the writeSarif call precedes `return r`).
