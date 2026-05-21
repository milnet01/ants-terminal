# ANTS-1719 — native auto-fix for mechanically-safe audit findings

## Problem

The Project Audit tool produces low-value, mechanically-fixable findings
(a dead `#include`, a `Q_UNUSED` whose variable is gone, a `TODO: remove
after <past-version>`, a `//uncommented` style nit). Today every such
finding is left for a human (or a Claude round-trip) to fix by hand.

The user's requirement (2026-05-21): *fix the fixable; surface the rest;
never silently bury anything.*

> Premise note: the original roadmap framing said low-confidence
> (≤ 30) findings were "marked suppressed and hidden." Verified against
> the code on 2026-05-21 — they are **not**: low-confidence findings
> already render (red pip), and `suppressed` is a separate
> fingerprint-keyed mechanism (`.audit_suppress` / learned-FP ledger),
> not a confidence threshold. So the "surface the rest" half is already
> satisfied; this feature delivers the **auto-fix engine** half.

## Solution

`ants::autofix` (`src/auditautofix.{h,cpp}`, Qt6::Core only) — a pure,
behaviour-neutral safe-list auto-fixer shared by the AuditDialog opt-in
action and this test. The dialog gets an opt-in **"Auto-fix safe (N)"**
button; auto-fix never runs implicitly on a scan.

### Exhaustive safe-list (each gated on a finding signal AND line shape)

| rule id | finding signal | line shape | repair |
|---|---|---|---|
| `autofix.unused_include` | checkId `cppcheck` + msg `unusedInclude` | `#include …` | remove line |
| `autofix.dead_q_unused` | msg names `Q_UNUSED` | standalone `Q_UNUSED(...);` | remove line |
| `autofix.stale_todo` | (line self-verifying) | `// … TODO/FIXME … remove after X.Y.Z` with X.Y.Z ≤ current | remove line |
| `autofix.comment_space` | msg mentions `comment` | standalone `//word` | `// word` |

Anything not provably one of these returns `nullopt` and is left for the
human/Claude — never auto-edited.

## Invariants

- **INV-1 — each safe-list rule round-trips.** `planRepair` returns the
  expected `Repair`, and `applyRepair` produces the expected file content
  for every safe-list case.
- **INV-2 — unsafe findings are never auto-fixed.** A future-version TODO,
  a cppcheck-unusedInclude finding whose line is *not* an `#include`, a
  `Q_UNUSED` finding on a non-`Q_UNUSED` line, a `//word` line whose
  finding does not mention "comment", and an out-of-range line all return
  `nullopt`.
- **INV-3 — applyRepair refuses a stale plan.** If the target line no
  longer equals `Repair.original`, `applyRepair` returns false and leaves
  the file unchanged.
- **INV-4 — the auto-fix log is append-only.** `logRepair` writes a
  header comment on first write and appends one JSON object per repair
  (`{file,line,rule,original,fixed,timestamp}`); a second call appends
  rather than truncating.
- **INV-5 — version comparison is conservative.** `versionLE` compares by
  numeric component; malformed input returns false (a malformed marker is
  never treated as removable).
- **INV-6 — applyRepair preserves the file's trailing-newline state** and
  only the targeted line changes.
