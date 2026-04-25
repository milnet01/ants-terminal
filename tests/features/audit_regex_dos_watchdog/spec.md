# Feature: regex-DoS watchdog on user-supplied audit patterns

## Contract

User-supplied regex patterns reach the audit dialog via two routes:

1. `audit_rules.json` → `OutputFilter::dropIfMatches` (per-rule
   post-filter regex applied in `applyFilter`).
2. `.audit_allowlist.json` → `AllowlistEntry::lineRegex` (compiled in
   `loadAllowlist` and matched against finding messages in
   `allowlisted`).

These run in the GUI thread (the audit pipeline is in-process for
custom grep rules). A pathological pattern matched against scanner
output — ripgrep, grep, custom commands — can pin the GUI thread for
seconds with classic catastrophic backtracking on adversarial input.

The audit dialog MUST defend against this by:

- Rejecting patterns whose **shape** is known-catastrophic (nested
  quantifiers like `(.+)+`, `(\w*)*`, `(.*)+`) at compile time. These
  are dropped with a `qWarning`; the rule continues to run without
  the offending filter rather than refusing to load.
- Wrapping every user-supplied pattern in PCRE2's `(*LIMIT_MATCH=N)`
  inline option so even a pattern that slips past the shape check
  cannot exceed a bounded match-step budget. PCRE2 (Qt6's
  `QRegularExpression` backend) returns "no match" when the limit is
  exceeded — fail-safe behavior.

## Rationale

The audit pipeline already validates that the pattern is parseable
(`QRegularExpression::isValid`) and emits a `qWarning` on bad
patterns. It does NOT validate against backtracking shape, and it
does not bound match-time. With user-controlled `audit_rules.json`
checked into a project, a malicious or accidentally-bad pattern can
DoS the GUI on every audit run.

The two-layer defense (shape-reject + step-limit) is
defense-in-depth: the shape check covers obvious catastrophes
(`(a+)+b` against `aaaaaaaaaaa!`), and the PCRE2 step-limit covers
everything else.

The step limit is set to 100,000 — handles all sane patterns
comfortably (a typical match completes in under 1,000 internal steps)
and aborts adversarial patterns within a few milliseconds even on
slow hardware.

## Invariants

**INV-1 — A regex-shape rejection helper exists and is invoked.**
Source-grep against `src/auditdialog.cpp`: a function whose name
contains a substring like `Catastrophic`, `Dangerous`, `RegexShape`,
`isCatastrophic`, etc. (case-insensitive) must be defined. It must
return a bool. The helper must be invoked at the user-pattern entry
points: somewhere referencing the `dropIfMatches` field AND
somewhere inside `loadAllowlist`.

**INV-2 — The shape-rejection helper recognizes nested quantifiers.**
Source-grep against `src/auditdialog.cpp`: the helper's body must
contain a regex literal that detects nested quantifiers — at minimum
a pattern containing both `[+*]` and another `[+*]` or `\+` separated
by characters (the sentinel for `(...+)+`-style shapes). We accept
any literal shape that includes the substring `[+*]` twice or both
`+` and `*` (case-insensitive) within a single regex literal.

**INV-3 — PCRE2 step-limit is applied to user patterns.**
Source-grep: the string `LIMIT_MATCH=` must appear in
`auditdialog.cpp` adjacent to the dropIfMatches compilation OR
inside a regex-hardening helper that's then called from the relevant
sites. The number after `LIMIT_MATCH=` must be a numeric literal in
[1000, 1000000] — within the bounds where match-time is bounded but
real patterns still run to completion.

**INV-4 — Bad patterns drop with a warning, not a crash.** Source-grep
inside the `loadAllowlist` body: a `qWarning` (or `qCritical`) call
that names the offending pattern when shape rejection fires. We
accept any `qWarning(... %s ...)` near the rejection code, paired
with the helper-invocation site (search window ±300 chars of the
helper call).

## Scope

### In scope
- Shape-detection helper (catastrophic backtracking heuristics).
- PCRE2 `LIMIT_MATCH` wrapper applied at user-regex entry points.
- `qWarning` on rejection.

### Out of scope
- Deep static analysis of regex AST (re2 / Hyperscan equivalence).
  The shape heuristic is a coarse first line of defense; the
  step-limit is the precise second line.
- Per-pattern timeout enforcement via thread cancellation. PCRE2's
  step counter is portable, deterministic, and cheaper than thread
  scaffolding.
- Bounding non-user-supplied regexes (e.g. the file:line:col parser
  regexes baked into `parseFindings`). Those are reviewed at coding
  time and are not adversarial input.

## Regression history

- **Pre-0.7.29:** user-supplied `dropIfMatches` and `line_regex`
  passed straight into `QRegularExpression` with no shape check and
  no step limit. A worst-case pattern committed in
  `audit_rules.json` could hang the GUI on every audit run.
- **0.7.29 (this fix):** shape-rejection + `(*LIMIT_MATCH=...)` prefix
  on user patterns; legitimate patterns unaffected.
