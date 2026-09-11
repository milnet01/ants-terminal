# Feature: an audit tool's whole process tree dies when the tool is stopped (ANTS-5038)

## Contract

Every audit tool the app runs — through `AuditRunner::runAudit` (headless
`audit_run`) or through `AuditDialog` (the GUI Audit dialog) — is stopped by
signalling only the direct child process the app spawned. A tool that forks
its own helpers (semgrep spawns `semgrep-core`; a check run as `/bin/bash -c
'<pipeline>'` forks whatever the pipeline forks) leaves those helpers running
after the app has decided the tool is stopped. The next check starts while
the previous one's helpers are still consuming CPU and memory.

The fix is to start every tool in its own process group and signal the whole
group, not just its leader. `src/processgroup.h` is the seam:
`ProcessGroup::startsOwnGroup(QProcess&)` is called before a tool's
`start()`; `ProcessGroup::signalGroup(process, sig)` is called at every site
that stops one, before the existing `terminate()` / `kill()` call, which
stays.

## Rationale

Found by three lanes. `cppcheck -j$(nproc)`, a whole-tree `clazy` run, and
the spawn-per-file checks routinely hit their timeout on a real tree, and
`semgrep` forks `semgrep-core`. On an earlyoom host, each run that leaves its
tool's helpers behind stacks their memory onto the next check.

## Invariants

- **INV-1 — the seam itself signals the whole group, not just the leader.**
  A process started via `ProcessGroup::startsOwnGroup` and stopped via
  `ProcessGroup::signalGroup` must have every process it forked stopped too,
  not just the leader. Test surface: `test_processgroup_helper.cpp` (drives
  a `/bin/sh` that forks a `sleep` and prints its pid, signals the group,
  and polls for the `sleep` pid to actually be gone).

- **INV-2 — `AuditRunner`'s per-tool cap stops the tool's whole process
  tree.** When a tool outlives its per-tool wall-clock cap, `runAudit` must
  report it `timed_out` (unchanged) AND the tool's own children must be gone
  by the time the run returns — not merely the tool's own QProcess. Test
  surface: `test_auditrunner_group_kill.cpp` (a fake tool forks a long
  sleeper, records its pid, and outlives a 5 s per-tool cap; the test polls
  for the sleeper to be gone after `runAudit` returns).

- **INV-3 — `AuditDialog` starts every check's process in its own group,
  and every site that stops one signals the group first.**
  `runNextCheck()` calls `ProcessGroup::startsOwnGroup(*m_process)` before
  `m_process->start(...)`. Every `m_process->kill();` call site (the
  per-check timeout, `cancelAudit()`, and the two output-overflow guards)
  is preceded, in the same statement block, by a
  `ProcessGroup::signalGroup(*m_process, ...)` call. Test surface:
  `test_auditdialog_group_kill_scrape.cpp` (source-anchored — the dialog is
  a `QDialog`; no test in this tree constructs one to drive a real check
  run).

## Scope

### In scope
- The two live call paths: `AuditRunner::runAudit` (headless) and
  `AuditDialog::runNextCheck` / its four kill sites (GUI).
- The seam itself, `src/processgroup.h`.

### Out of scope
- `AuditDialog`'s behaviour end to end (it is a `QDialog`; this project's
  house pattern for it is source-scrape, not construction — see
  `audit_dialog_render_hardening`, `audit_dialog_v2`).
- Any specific external tool (`cppcheck`, `clazy`, `semgrep`, ...). The fix
  is generic to how a `QProcess` is started and stopped; INV-2 exercises it
  through a fake tool for exactly that reason.

## Regression history

- **ANTS-5038 (found by three lanes):** `AuditRunner::runAudit`'s per-tool
  cap and `AuditDialog`'s four kill sites each signal only the direct child;
  `src/processgroup.h` ships as a stub (`startsOwnGroup` a no-op,
  `signalGroup` a plain `kill(pid, sig)`) pending the fix
  (`setChildProcessModifier` + `setsid()` on start, `kill(-pid, sig)` to
  signal). This spec locks the contract before the fix lands; all three
  invariants are expected to fail against the stub.
