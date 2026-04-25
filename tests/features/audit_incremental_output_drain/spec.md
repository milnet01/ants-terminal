# Feature: incremental QProcess output drain with size cap

## Contract

`AuditDialog`'s tool runner MUST drain `QProcess` output incrementally
via `readyReadStandardOutput` / `readyReadStandardError` slots,
accumulating into a bounded byte buffer. On `finished()`, the audit
pipeline reads the accumulated buffer rather than calling
`m_process->readAllStandardOutput()` once at end. If the buffer
exceeds a hard cap (`MAX_TOOL_OUTPUT_BYTES`, currently 64 MiB), the
process MUST be killed and a tool-health warning recorded — not
silently allowed to grow until the GUI process OOMs.

The previous implementation called `readAllStandardOutput()` exactly
once inside `onCheckFinished`. Until that moment, the kernel-side
pipe buffer (typically 64 KB on Linux, larger on FreeBSD) would
back-pressure the tool, but `QProcess` itself accumulates internal
buffers without bound — a runaway `semgrep` on generated code or a
buggy plugin emitting a tight `printf` loop could buffer hundreds of
megabytes of process-side data before the timeout fired, with the
audit dialog showing a frozen progress bar the whole time.

## Rationale

External tools occasionally produce far more output than expected:

| Failure mode | Realistic trigger |
|--------------|-------------------|
| Recursive grep finds a hot loop pattern | `for { print }` over a generated Boost-heavy TU |
| Tool prints debug to stderr by default | older `semgrep` versions; some `pylint` plugins |
| Tool error-loops on bad input | `clang-tidy` against a generated file emits one diagnostic per macro expansion |
| Tool intentionally produces large output | `clazy --extra-options=fixit` emits per-fixit blobs |

Without incremental drain + a cap:
1. `QProcess`'s read buffer grows. Default `setReadBufferSize(0)` =
   unbounded.
2. The runner blocks waiting for `finished()`, which only fires when
   the tool actually exits.
3. A pathological tool can buffer 500 MB before the 30 s timeout
   triggers — that's 500 MB sitting in `QProcess` plus another 500 MB
   when `readAllStandardOutput()` allocates the result `QByteArray`.

With incremental drain + cap:
1. Each `readyRead*` fire appends a chunk to `m_currentOutput` /
   `m_currentError`, with a size check.
2. If size > cap, kill the process and record a clear "tool-health"
   warning that names the cap — distinct from the "Timed out" path.
3. On `finished()`, the audit pipeline reads from the member buffers
   instead of the live process — same data flow downstream, just
   pre-bounded.

64 MiB is large enough that any realistic tool output fits (the
biggest real-world burst observed is ~8 MiB from a `cppcheck` run on
a 500k-line codebase with verbose suppressions); small enough that
two pathological accumulators can't OOM a 4 GB GUI process.

## Invariants

**INV-1 — `readyReadStandardOutput` and `readyReadStandardError` are
connected.** Source-grep against `src/auditdialog.cpp`: the constructor
must connect both signals on `m_process` to dialog slots. Without
incremental wiring, the runner reverts to one-shot read.

**INV-2 — Member buffer for accumulated output.** Source-grep against
`src/auditdialog.h`: a member of byte-buffer type
(`QByteArray` / `std::string`) named with the prefix `m_currentOutput`
or `m_currentError` (or both). Distinct from any per-check
`CheckResult` struct field.

**INV-3 — `MAX_TOOL_OUTPUT_BYTES` cap declared.** Source-grep
against `src/auditdialog.h` or `src/auditdialog.cpp`: a numeric
constant named `MAX_TOOL_OUTPUT_BYTES` with a value of at least
4 MiB (`4 * 1024 * 1024`). Lower caps would chop legitimate cppcheck
output; higher than 1 GiB would defeat the purpose.

**INV-4 — Buffers reset before each check.** Source-grep: a `clear()`
call (or `= {};` reassignment) on the buffer member must appear
inside `runNextCheck` before the `m_process->start` invocation, so
output from check N+1 doesn't concatenate with check N's tail.

**INV-5 — `onCheckFinished` reads from the buffers, not the live
process.** Source-grep: `onCheckFinished` body must reference
`m_currentOutput` / `m_currentError` (or whatever name was chosen
for the accumulators). The literal `m_process->readAllStandardOutput()`
must NOT appear in `onCheckFinished` — that's the pre-fix pattern
this spec replaces.

**INV-6 — Overflow path kills the process and records a warning.**
Source-grep: the drain slot must contain a comparison against
`MAX_TOOL_OUTPUT_BYTES` and call `m_process->kill()` on overflow.

## Scope

### In scope
- Source-grep regression test pinning the connections, members, cap,
  reset, finished-handler change, and overflow path.

### Out of scope
- Behavioral test that runs a tool which intentionally produces > 64
  MiB. Would require a synthetic spammy command and a real
  reproducible harness — disproportionate for the structural fix.
- Per-check tunable cap (a future enhancement). Single global cap is
  enough for the failure modes observed.
- Streaming the buffer out to a temp file at overflow rather than
  killing. Considered, rejected: temp files complicate cleanup, and
  any tool producing > 64 MiB is broken — record the breakage, don't
  hide it.

## Regression history

- **Original implementation:** `m_process->readAllStandardOutput()`
  in `onCheckFinished`. No incremental drain. No cap. A runaway
  semgrep against generated code could buffer ~500 MB before the
  timeout caught it.
- **0.7.12 ROADMAP entry (Tier 2 hardening):** flagged at
  `auditdialog.cpp:3747-3765` — "reads `readAllStandardOutput()`
  once; a runaway semgrep on generated code can buffer hundreds of
  MB before the timeout. Stream to a temp file with a size cap."
  This fix uses an in-memory bounded buffer rather than a temp
  file (rationale in Scope above).
- **0.7.28 (this fix):** incremental drain + 64 MiB cap +
  kill-on-overflow.
