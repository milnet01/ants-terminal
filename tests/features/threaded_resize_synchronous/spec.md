# Threaded resize synchronous

**Status:** locked in 0.7.0.

## Invariant

When the threaded parse path is active (`ANTS_PARSE_THREAD=1`), every
`TerminalWidget::resizeEvent`-driven `VtStream::resize` invocation must
use `Qt::BlockingQueuedConnection`, not `Qt::QueuedConnection`. The
blocking variant guarantees the PTY winsize is updated before
`resizeEvent` returns, so the next paint always sees consistent
geometry.

A naive refactor might change this to `QueuedConnection` ("it's only a
resize, who cares?") — and introduce a 1-frame flicker where the grid
repaints at the new size while the child process is still writing at
the old cols/rows. This test pins the guarantee at the source level.

## What we test

A source-level inspection of `src/terminalwidget.cpp`:

1. Every `QMetaObject::invokeMethod(m_vtStream, "resize", ...)` call
   must use `Qt::BlockingQueuedConnection`.
2. The `startShell` init path's `resize` invocation must also use
   `Qt::BlockingQueuedConnection` — it's part of the same contract
   (grid and PTY dimensions match before the widget first paints).
3. Correspondingly, `VtStream::resize` itself must be declared as a
   slot (`public slots:` block) in `src/vtstream.h` — otherwise
   `invokeMethod` by string name would silently fail. Grep for the
   declaration.

## Scope

Pure source-grep test. No Qt widgets, no thread — the contract lives
at the source level, checkable by static inspection. Same pattern as
`tests/features/shift_enter_bracketed_paste`.
