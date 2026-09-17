# audit_run_output_cap — the headless audit runner bounds tool output

**Bundle:** `test_audit` · **Suite:** `AuditRunOutputCap` · **Label:** `features;fast`

## Problem

`AuditRunner::runAudit` read each tool's output with
`readAllStandardOutput()` and `readAllStandardError()` once the tool exited.
`QProcess` buffers output without limit until then, so a tool printing in a
loop grew the Ants process without bound. The raw output was then copied into
the SARIF `toolExecutionNotifications` message, up to `kSarifFindingsMax × 256`
characters per tool, against the ~7 MiB SARIF budget in
`docs/specs/ANTS-1351.md` § 6.

## Surface

- The runner drains each tool's stdout and stderr as they arrive. When one
  tool's combined output passes `kMaxToolOutputBytes` (64 MiB, the audit
  dialog's `MAX_TOOL_OUTPUT_BYTES`), the runner stops the tool's process group
  and records status `output_too_large`. Its partial output is not parsed.
- `output_too_large` is a fourth `ToolResult::status`. It counts as incomplete,
  and `incompleteToolsDetail` marks it `truncated`, like `timed_out`.
- A SARIF notification carries at most `kSarifNotificationMaxChars` (64 KiB) of
  a tool's raw output.

## Invariants

- **INV-1 — runaway output is capped.** A fake `shellcheck` printing 70 MiB to
  stdout ends with status `output_too_large`.
- **INV-2 — the SARIF excerpt is bounded.** A fake `shellcheck` printing 1 MiB
  produces a SARIF whose `toolExecutionNotifications` message is at most
  64 KiB.
- **INV-3 — the capped status is truncated.** Covered in
  `tests/features/audit_run_incomplete_detail` INV-11: `incompleteToolsDetail`
  reports `truncated: true` for `output_too_large`.

## Test surface

A fake tool placed first on `PATH`, as in
`tests/features/audit_tool_process_group_kill`. `shellcheck` is used because no
other test in the bundle fakes it, so the runner's per-process tool cache holds
nothing stale for it.

## Regression history

- **ANTS-5085:** tool output was read whole with no byte cap and embedded in
  the SARIF by the megabyte. New status decided by the user, 2026-09-17.
