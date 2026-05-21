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

## Test notes

GUI bundle (needs QApplication for the QDialog). Drives a minimal concrete
subclass; `dispatchOne` uses an injected fake runner. Label
`features;fast`.
