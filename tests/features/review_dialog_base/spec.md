# Feature: ReviewDialogBase — shared review-dialog scaffold

## Problem

ANTS-1727 § 2.4 adds `ReviewDialogBase` — the shared QDialog scaffold the
v2 review dialogs (ColdEyes ANTS-1721, TestAudit ANTS-1722) subclass. It
owns the partition panel, brief tabs, the Dispatch button (→
`LlmDispatcher`), a results host, and the Fold-into-ROADMAP button;
subclasses fill four hooks and use the base services.

## Invariants under test (ANTS-1727)

- **INV-12** — `endpointDispatchable` is true iff the endpoint is non-empty
  AND http/https; Dispatch is disabled when false.
- **INV-13** — `allocateFoldInIds(n)` returns `[]` and surfaces a counter
  reason on a counter failure, writing nothing.
- **INV-15** — `dispatchOne` runs a single follow-up job and invokes its
  callback without firing `onAllReportsCollected` (so a synthesis job does
  not re-enter the batch-complete path).
- **INV-19** (ANTS-2111) — the dialog tracks the `LlmClient`s its runner
  spawns and aborts them in `~ReviewDialogBase` *before* `m_dispatcher->
  cancelAll()`, so closing the window mid-review cannot deliver a
  synchronous `finished()` (from `~LlmClient`'s reply abort) into a
  half-destroyed dialog. Source-scrape: the teardown race is GUI-/network-
  bound and not reproducible offscreen without a live reply.
- **INV-20** (regression) — `onJobFinished` stores a job's `result.text`
  in `reports()` only when `result.ok` is true; a failed job's id is
  removed from (or never inserted into) `reports()` — including dropping
  a stale report a prior successful round left behind — and once the
  round finishes the status label names each failed lane and contains
  "failed", so a resume/retry flow can tell a failure from a genuinely
  empty report.
- **INV-21** (regression) — starting a dispatch round (`startDispatch` or
  a non-empty `redispatch`) disables the "Dispatch to AI" button; it
  re-enables once the round finishes (when the endpoint is still
  dispatchable), so a second click mid-round cannot re-run `startDispatch`
  and clear in-flight `reports()` out from under a round already running.
- **INV-22** (regression) — `startDispatch` with no lanes starts no round:
  it neither disables Dispatch nor calls `onAllReportsCollected`, matching
  the guard `redispatch` already has (`if (!jobs.isEmpty())`). Depends on
  `LlmDispatcher::enqueue` ignoring an empty list (INV-15 in
  `tests/features/llm_dispatcher/spec.md`).

## Test notes

GUI bundle (needs QApplication for the QDialog). Drives a minimal concrete
subclass; `dispatchOne` uses an injected fake runner. INV-21/INV-22
construct a real `Config` (XDG-sandboxed by the bundle's `main()`, so no
write touches the user's real config.json) with an `ai_endpoint` set, so
`endpointDispatchable` passes and the Dispatch button starts enabled; they
locate the button by its `"Dispatch to AI"` label since it has no
dedicated accessor. Label `features;fast`.
