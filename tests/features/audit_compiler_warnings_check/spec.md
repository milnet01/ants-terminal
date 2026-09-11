# Feature: the `compiler_warnings` audit check can finish and cleans up after itself (ANTS-5039)

## Contract

The `compiler_warnings` check (`src/auditdialog.cpp`, raw `m_checks.append`
entry) configures and builds this project from scratch in a scratch
directory, then greps the build output for warnings and errors. That is a
several-minute job on a non-trivial project, but the check carries no
explicit `timeoutMs`, so it inherits `AuditCheck::timeoutMs`'s 30 s default
and is killed on every run before `cmake --build` can finish — the check
can never report a warning.

Since ANTS-5038, the dialog's per-check timeout and its Cancel button both
signal the whole process group (SIGTERM, then SIGKILL after a grace
period) rather than just the direct child. The command's own cleanup is a
trailing `rm -rf "$tmpdir"` after the build — code the SIGKILL never lets
run. So every attempt (and, separately, every user Cancel) leaves behind
the `mktemp -d` build tree it was working in, plus whatever `cmake`,
`ninja` and compiler child processes were still writing into it when the
group was signalled.

The fix: an explicit `timeoutMs` sized for a real build, a `trap ... EXIT`
that removes the scratch directory however the shell exits, and a single
fixed per-user scratch path (cleared before the run starts, so a tree a
kill leaves behind is reclaimed by the very next run) instead of a fresh
`mktemp -d` name every time.

## Rationale

Filed as ANTS-5039. Reported symptom: the check never returns a single
warning, and `/tmp` accumulates orphaned build trees plus the orphaned
`cmake`/`ninja`/compiler processes still writing into them (see the
companion orphaned-process roadmap item). Both trace to the same root
cause — the 30 s timeout is unreachable for a from-scratch build of this
project, and the only cleanup path is code that a SIGKILL never lets run.

## Invariants

- **INV-1 — the check declares an explicit, minutes-scale timeout.** The
`m_checks.append` block for `compiler_warnings` carries a `timeoutMs`
reaching at least a few minutes — either a trailing integer in the
aggregate initializer or an assignment naming `timeoutMs` for this check —
rather than inheriting `AuditCheck::timeoutMs`'s 30000 ms default. Test
surface: `test_audit_compiler_warnings_check.cpp`,
`AuditCompilerWarningsCheck.ExplicitLongTimeout` (source-scrape: scans the
block for the largest integer literal, or product of an adjacent-literal
multiplicative expression, and requires it to clear a 3-minute floor).
Current: red — the block's largest such value is `60` (the `OutputFilter`
`maxLines` field).

- **INV-2 — the command registers a `trap` on `EXIT` that removes the
scratch directory.** However the shell exits — normal completion, the
per-check timeout's SIGTERM/SIGKILL, or a user Cancel — the scratch
directory it built in is removed. Test surface:
`AuditCompilerWarningsCheck.TrapCleansUpScratchDirOnExit` (source-scrape:
looks for `trap` followed by `EXIT`, with an `rm` call between them).
Current: red — the command has no `trap` at all; its only cleanup is a
trailing `rm -rf "$tmpdir"` that a kill never reaches.

- **INV-3 — the build directory is not created with `mktemp -d`.** A fresh
random name every run cannot be found and cleared by a later run when the
current one is killed mid-build. Test surface:
`AuditCompilerWarningsCheck.NoMktempDForBuildDir` (source-scrape: asserts
`mktemp -d` does not appear in the command). Current: red — the command
reads `tmpdir=$(mktemp -d)`.

- **INV-4 — the `[ -f CMakeLists.txt ]` guard is still a no-op when the
directory has no `CMakeLists.txt`.** Run in an otherwise-empty directory,
the command must exit 0 and print nothing. This is unrelated to what
ANTS-5039 is fixing (the guard already exists and already works) and
exists to lock it against a rewrite of the command breaking it by
accident. Test surface:
`AuditCompilerWarningsCheck.NoCMakeListsGuardIsANoOp` (behavioural: the
command field is extracted from source as a run of adjacent string
literals and run with `bash -c` — the same invocation shape
`m_process->start("/bin/bash", {"-c", check.command})` uses — in a fresh
directory with no `CMakeLists.txt`; skipped, with a stated reason, if the
extraction does not come back as a clean literal run). Current: expected
green — this half of the command is not what ANTS-5039 changes.

## Scope

### In scope
- The `compiler_warnings` check's `timeoutMs` and its command string, both
  in the `m_checks.append` block in `src/auditdialog.cpp`.

### Out of scope
- The orphaned-process cleanup for a tree already left behind by a past
  run — a separate roadmap item, referenced from ANTS-5039's own
  description.
- Any other audit check's timeout or scratch-directory handling.
- Constructing `AuditDialog` to drive a real check run — this project's
  house pattern for it is source-scrape (see `audit_dialog_render_hardening`,
  `audit_tool_process_group_kill`), because `AuditDialog` is a `QDialog`.
- Actually running a from-scratch build under the fixed check — INV-1
  through INV-3 lock the command's shape; INV-4 is the only invariant that
  executes the extracted command, and only its cheap, guard-only path.

## Regression history

- **ANTS-5039:** the check has no explicit `timeoutMs` (inherits the 30 s
  default), builds in a fresh `mktemp -d` directory with cleanup only as a
  trailing `rm -rf` a kill never reaches, and never returns a warning on a
  non-trivial project. This spec locks the contract before the fix lands;
  INV-1 through INV-3 are expected to fail against the current tree.
