# Closing the audit dialog mid-check (ANTS-6539)

The audit dialog is opened with `WA_DeleteOnClose`, so closing it while
a check runs destroys it. Its members die before `~QWidget` deletes the
child `QProcess` objects. `~QProcess` kills a running child, waits for
it, and emits `finished` on the way, into slots of a dialog whose
members are already gone.

## Invariant

- **INV-1** — Deleting the dialog while one of its processes runs emits
  no signal from that process during the deletion. `~AuditDialog`
  un-parents every child `QProcess` and hands it to `releaseProcess`,
  which kills it and deletes it once it has finished. By then `~QObject`
  has dropped the dialog's connections.

## Test

`test_audit_dialog_close_mid_check.cpp`, in the `test_dialogs` bundle.
It starts `sleep` on the dialog's own `QProcess`, deletes the dialog,
and asserts that `finished` did not fire during the deletion and that
the process is deleted once events run.
