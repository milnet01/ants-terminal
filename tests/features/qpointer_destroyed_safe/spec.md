# ANTS-1324 — UBSan-safe terminal-list compaction

## Background

`MainWindow` keeps a flat list of every active `TerminalWidget`
(`m_allTerminals`, `QList<QPointer<TerminalWidget>>`) so the
broadcast / iterate-all sites don't each walk Qt's child tree.

Two earlier attempts at maintaining this list both hit
sanitizer-grade bugs:

1. **ANTS-1320 (HUAF, ASan-confirmed 2026-05-13).** `connectTerminal`
   connected a `destroyed` slot that walked `m_allTerminals` from
   inside Qt's child-auto-destruction during `~MainWindow`. The
   slot ran AFTER `m_allTerminals` had already been destroyed by
   `~MainWindow`'s member-destruction chain → glibc "corrupted
   double-linked list" SIGABRT on release builds, clean ASan HUAF
   on the sanitized build.

2. **ANTS-1324 (UBSan, vptr, 2026-05-14).** The ANTS-1320 fix
   thought it had avoided the bad downcast by casting the *result*
   of `p.data()` back UP to `QObject*` inside the predicate. But
   `QPointer<TerminalWidget>::data()` itself IS the downcast —
   `static_cast<TerminalWidget*>(QObject*)` at `qpointer.h:75`.
   The upcast came too late; UBSan -fsanitize=vptr catches the
   downcast in `.data()`, not the use afterwards. The bug fires
   whenever any `TerminalWidget` is destroyed via deferred-delete
   (e.g. `shellExited` → `deleteLater()`, or shutdown-time SIGTERM
   → shell-exit chain).

## Why downcasts during ~QWidget() are UB

When `~TerminalWidget()` runs, Qt destruction order is:

```
~TerminalWidget()   ← vptr = TerminalWidget
  ~QWidget()        ← vptr demoted to QWidget HERE
    emit QObject::destroyed(this)   ← slots run with vptr = QWidget
  ~QObject()        ← vptr demoted to QObject
```

Any `QPointer<TerminalWidget>::data()` called between the second
and fourth line above invokes `static_cast<TerminalWidget*>(QObject*)`
on an object whose vptr is QWidget. UBSan flags it.

Qt's QPointer auto-null fires AFTER `destroyed()` returns — so
during the slot, the QPointer still holds the (now-typewise-stale)
pointer. Reading it via `.data()` is the unsafe step.

## Contract

**I-1** — `MainWindow::connectTerminal` MUST NOT install a
`QObject::destroyed` slot that calls
`QPointer<TerminalWidget>::data()` (directly or via implicit
conversion). Any such slot runs while `~QWidget()` is on the
stack — UB per the chain above.

**I-2** — `MainWindow::liveTerminals() const` MUST compact null
entries from `m_allTerminals` before iterating. The Qt-auto-null
safety net only nulls each entry slightly after destroyed()
returns, so a lazy read-time compact is the correct point.

**I-3** — `m_allTerminals` MUST be declared `mutable` so I-2 fits
inside the existing `const` accessor.

**I-4** — `~MainWindow()` MUST NOT iterate `m_allTerminals` to
disconnect destroyed handlers. The ANTS-1320 disconnect loop
existed to prevent its HUAF; the present design connects no such
handler, so the loop would be a no-op.

**I-5 (regression-lock)** — `src/mainwindow.cpp` MUST NOT contain
the pattern `connect(<x>, &QObject::destroyed, this, …)` whose
lambda body references `m_allTerminals`. Direct grep enforced by
this test.

## Test

`test_qpointer_destroyed_safe.cpp` performs three static checks
against `src/mainwindow.cpp` + `src/mainwindow.h`:

- **C-1 (I-5)**: no occurrence of `m_allTerminals.removeIf` inside
  any `&QObject::destroyed,` connection block.
- **C-2 (I-3)**: `m_allTerminals` is declared `mutable`.
- **C-3 (I-2)**: `liveTerminals` contains `removeIf` with a
  null-predicate.

Pure grep — no build-time runtime dependency. Fast.
