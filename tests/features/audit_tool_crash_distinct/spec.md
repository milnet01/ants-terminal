# Feature: tool crash is distinct from "no findings"

## Contract

`AuditDialog::onCheckFinished` MUST examine the `QProcess::ExitStatus`
parameter and the exit code, and produce a tool-health warning
(distinct from the "no findings" silent-pass) when:

1. `status == QProcess::CrashExit` — the tool received a signal
   (segfault, SIGABRT, killed by oom-killer, etc.); OR
2. `exitCode != 0` AND the accumulated stdout is empty AND stderr
   contains diagnostic text (i.e. the tool errored out before
   producing any findings).

The warning must name the failure mode (signal vs non-zero exit) and
include any stderr text the tool produced. The pre-fix implementation
ignored the `ExitStatus` parameter (`/*status*/`) and also ignored
the exit code — a tool that segfaulted with empty stdout was
indistinguishable from a clean run with zero findings, silently
hiding both a real bug in the tool AND any findings the tool would
have reported if it hadn't crashed.

## Rationale

`QProcess::finished(int exitCode, QProcess::ExitStatus status)` carries
two pieces of failure information. The pre-fix code's signature

```cpp
void AuditDialog::onCheckFinished(int /*exitCode*/, QProcess::ExitStatus /*status*/)
```

discarded both. The result was three failure modes collapsing into
one observation:

| What actually happened | What the audit report showed |
|------------------------|------------------------------|
| Tool ran cleanly, found no issues | "0 findings" (correct) |
| Tool segfaulted before producing output | "0 findings" (silent) |
| Tool exited 1 with `error: cannot open foo.cpp` on stderr | "0 findings" (silent) |
| Tool exited 0 with stderr "warning: …" | "0 findings" (silent — stderr was discarded if stdout had content; merged otherwise) |

The third case is especially insidious: a misconfigured `clang-tidy`
that can't find Qt headers reports nothing useful, and the audit
report claims the project is clean. This violates the audit
pipeline's reason for existing.

Distinguishing the cases is mechanical: `CrashExit` covers signals,
non-zero exit covers tool-internal errors, the "stdout empty +
stderr non-empty" combination distinguishes "tool failed" from
"tool succeeded with diagnostics on stderr."

## Invariants

**INV-1 — `onCheckFinished` parameters are named, not commented out.**
Source-grep against `src/auditdialog.cpp`: the function signature
must read `void AuditDialog::onCheckFinished(int exitCode,
QProcess::ExitStatus exitStatus)` (or some variant where both
parameters are named identifiers, not `/*…*/` discards). The
pre-fix `int /*exitCode*/` / `QProcess::ExitStatus /*status*/`
pattern must not match.

**INV-2 — `QProcess::CrashExit` is referenced inside the function
body.** Source-grep: the body of `onCheckFinished` must contain
`QProcess::CrashExit` (or `CrashExit` after a `using QProcess::ExitStatus`
shorthand). This is the signal-detection branch.

**INV-3 — The crash branch produces a warning with `warning = true`
and a distinguishing message.** Source-grep: a `r.warning = true`
assignment inside the function body must be reachable from the
`CrashExit` branch (or the non-zero-exit branch), with an output
string distinct from the success path. We grep loosely — the
exact phrasing isn't pinned, but a string containing "crashed"
or "exit" must appear in proximity to the warning assignment.

**INV-4 — Severity demoted to Info on tool-health warnings.**
Source-grep: the warning row's `r.severity` must be set to
`Severity::Info` (or equivalent), matching the existing convention
for the timeout warning. Without this, a Major-severity check that
crashed would inherit Major severity and pollute the severity-sorted
list.

## Scope

### In scope
- Source-grep regression test pinning the parameter naming,
  `CrashExit` reference, warning emission, and Info severity.

### Out of scope
- Behavioral test that segfaults a real binary. Would need a
  synthetic crash-on-stdin harness — disproportionate for the
  structural fix.
- Stderr-only tool reporting (e.g. tools that only produce stderr
  on success). Not a real failure mode for the tools we ship; the
  existing stderr-merge path handles tools that produce stderr +
  zero stdout when exit code is 0.
- Re-running crashed tools automatically (transient signal
  handling). Out of scope; the warning surfaces the issue and lets
  the user re-run if desired.

## Regression history

- **Original implementation:** `void AuditDialog::onCheckFinished(int
  /*exitCode*/, QProcess::ExitStatus /*status*/)` — both parameters
  discarded. Crashed tools indistinguishable from clean runs.
- **0.7.12 ROADMAP entry (Tier 2 hardening):** flagged at
  `auditdialog.cpp:3747` — "currently treats segfaults (stderr
  'Segmentation fault') as findings" (or equivalent silent
  no-findings, depending on whether stderr was empty).
- **0.7.28 (this fix):** parameters named, `CrashExit` branch
  surfaces a warning, non-zero-exit-with-empty-stdout-but-stderr
  pattern likewise.
