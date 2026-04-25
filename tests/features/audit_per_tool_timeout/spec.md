# Feature: per-tool timeout override on AuditCheck

## Contract

Every `AuditCheck` carries its own `timeoutMs` field. The audit runner
MUST start `m_timeout` with the per-check value, not a global hard-coded
constant. Check definitions that wrap genuinely slow tools (cppcheck on
the full tree, semgrep with a rule pack, osv-scanner over the dependency
graph, trufflehog over the git history) MUST set `timeoutMs` above the
30 s default so they don't false-positive as "Timed out — tool-health
issue" on real-world projects.

The previous implementation hard-coded `m_timeout->start(30000)` in
`runNextCheck` and the timeout-handler lambda printed a literal
`"Timed out (30s)"` warning. The 30 s cap was set conservatively for
the fast in-process / grep-based checks; external static analysers
running over 500k-line codebases routinely blow past it, polluting
the report with tool-health warnings and silently dropping any
findings the tool would have produced if allowed to finish.

## Rationale

The audit dialog runs ~30 checks. Most are fast (grep, find, awk
pipelines that finish in < 1 s). A handful are slow:

| Tool | Typical wall time on this project | Why slow |
|------|-----------------------------------|----------|
| `cppcheck` (full tree) | 25-60 s | AST build per TU, `-j$(nproc)` parallelism but per-TU latency dominates |
| `cppcheck_unused` (single-threaded) | 30-90 s | `--enable=unusedFunction` requires whole-program analysis, can't parallelize |
| `clang-tidy` | 20-50 s | re-parses every TU; gets worse on Qt-heavy code |
| `clazy` | 20-40 s | clang-based AST walk; bounded by TU count |
| `semgrep` | 30-90 s | rule-pack compile + AST traversal across language tree |
| `osv-scanner` | 60-180 s | network-bound (OSV.dev rate limits) |
| `trufflehog` | 60-150 s | full git-history scan, expensive regex eval |

A single global 30 s cap means the slow tools either (a) get killed
mid-run and produce a tool-health warning instead of findings, or (b)
get auto-deselected by users frustrated with the false timeouts —
defeating the audit pipeline's purpose.

The fix is structural: per-check timeout, with reasonable defaults
baked into the check definitions for the known-slow tools.

## Invariants

**INV-1 — `AuditCheck` declares an `int timeoutMs` field with a
default value.** Source-grep against `src/auditdialog.h`: a member
declaration matching `int\s+timeoutMs\s*=\s*\d+` must appear inside
the `struct AuditCheck { … };` body. The default value MUST be the
30000 ms previously used as a global constant so existing positional
aggregate-init call sites that don't name the field stay correct.

**INV-2 — `runNextCheck` starts the timer with the per-check value.**
Source-grep against `src/auditdialog.cpp`: the `m_timeout->start(…)`
call inside `runNextCheck` must read its argument from the current
`AuditCheck` instance (regex `m_timeout->start\(\s*check\.timeoutMs\)`
or `\.timeoutMs` adjacent to the start call). Hard-coded literals
(`m_timeout->start(30000)`) inside `runNextCheck` are forbidden.

**INV-3 — Timeout-handler warning message reflects the actual cap.**
The lambda connected to `m_timeout->timeout()` must format the cap
into the warning string rather than printing a literal `"30s"`.
Source-grep: the previous literal `"Timed out (30s)"` must not appear
verbatim; the warning construction must reference a numeric variable
or computed value.

**INV-4 — At least one known-slow tool gets a > 30 s override.**
Source-grep: at least one of `cppcheck`, `semgrep`, `osv_scanner`,
`trufflehog`, `clang_tidy`, or `clazy` must appear adjacent (within
±2 lines) to a `timeoutMs = ` assignment whose value is > 30000.
This locks the calibration intent so a future refactor that strips
the per-check defaults gets caught.

## Scope

### In scope
- Source-grep regression test pinning the struct field, the use site,
  the message-format change, and at least one calibration override.

### Out of scope
- Behavioral test that runs an actual long-running tool. Would require
  a real cppcheck/semgrep install on the CI host and an
  intentionally-slow target — disproportionate for the structural
  invariant.
- User-visible UI for editing per-tool timeouts at runtime. The
  defaults are baked in via `populateChecks`; future enhancement
  could surface them in the Settings dialog.
- Percentile-based auto-tuning (e.g. learn from prior runs). Out of
  scope; defaults are static.

## Regression history

- **Original implementation:** `m_timeout->start(30000)` hard-coded
  in `runNextCheck`. Timeout-handler message hard-coded `"Timed out
  (30s)"`. Slow tools (cppcheck, semgrep, osv-scanner) routinely
  triggered the warning and were eventually auto-deselected by users.
- **0.7.12 ROADMAP entry (Tier 2 hardening):** flagged at
  `auditdialog.cpp:3729` — "global 30 s timeout; cppcheck
  --enable=all on a 500k-line codebase or osv-scanner rate-limited
  by OSV.dev both blow this. Add `timeoutMs` to `AuditCheck`."
- **0.7.28 (this fix):** `timeoutMs` field on `AuditCheck`, per-check
  start, calibrated overrides for known-slow tools.
