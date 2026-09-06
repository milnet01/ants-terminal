# ANTS-3846 — a tool that printed its usage did not find anything

## Background

Found in the audit report of 2026-08-06. `clang-tidy` ran with no check
selection, so it enabled no checks, printed `Error: no checks enabled.`
followed by its full `USAGE:` banner, **and exited 0**. The runner parsed
that banner as tool output, and the report carried

> clang-tidy Analysis (smell / C/C++) [clang-tidy] — 92 finding(s)

whose contents were lines like `--help  Display available options`.

Two harms, and the second is the one this item is about. The tool
contributed no analysis — that half is closed, because ANTS-4787 shipped
a root `.clang-tidy`. **The second harm is structural and still open: a
tool that produced nothing is indistinguishable in the report from a tool
that found 92 things.** That is strictly worse than the tool being
absent, because absent reads as absent and this reads as coverage.

The exit code cannot be used to detect it: verified 2026-09-06 on this
machine, `clang-tidy --checks=-* a.cpp --` prints the banner and exits
**0**.

## The precedent this follows

ANTS-3395 added `hasToolAbortMarker` for the JSON tools: a tool that
logged a fatal abort and produced no parseable findings sets
`ParsedOutput.aborted`, which the runner promotes to a `crashed` status
so the tool lands in `incomplete_tools[]` instead of reading as a clean
run. The plain-text tools (cppcheck / clazy / clang-tidy / mypy) have no
equivalent. This gives them one.

## Invariants

### INV-1 — a usage banner is an abort, not findings

Plain-text tool output whose head carries a `USAGE:` banner, or the
`Error: no checks enabled.` line, parses to zero findings and sets
`aborted`. The fixture is the tool's real bytes, captured from the
installed `clang-tidy`.

### INV-2 — the guard is conservative

The markers are matched at line start and only near the head of the
stream, so a finding whose MESSAGE mentions usage is not swept up. A
normal findings stream is unaffected: same counts as before, `aborted`
false.

### INV-3 — it does not fire on the JSON tools

Those already have ANTS-3395's marker and their own parse path; this
guard is for the line-based tools only.
