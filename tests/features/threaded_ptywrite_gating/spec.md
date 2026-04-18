# Threaded ptyWrite gating

**Status:** locked in 0.6.35 after regression.

## Invariant

Under the threaded parse path, `m_pty` is **null** on `TerminalWidget`
(the Pty lives on the worker thread, owned by `VtStream`). Any
keystroke / paste / response-write gate of the form
`if (m_pty) ptyWrite(...)` or `if (cond && m_pty) ptyWrite(...)` will
silently swallow every call because the bare-pointer check fails —
even though `ptyWrite()` itself handles both paths correctly.

The contract is:

- Inside `TerminalWidget`'s implementation file, `m_pty` may only be
  mentioned in the legacy-path code (`startShell`'s `!useWorker`
  branch; `recalcGridSize`'s legacy resize branch; inside the
  `ptyWrite` / `ptyChildPid` helpers themselves).
- Any other gate must use `hasPty()` (returns true for either path)
  or simply call `ptyWrite(...)` unconditionally (the helper is
  null-safe).

## What we test

A source-level inspection of `src/terminalwidget.cpp`:

1. Every `ptyWrite(` call must NOT be preceded on the same line by
   `if (m_pty)` or guarded by `&& m_pty)`.
2. Any `&& m_pty)` or `!m_pty` pattern that isn't inside the allowlisted
   functions fails the test.

Allowlist: `ptyWrite`, `ptyChildPid`, `startShell`, `recalcGridSize`,
and the constructor/destructor for m_pty lifecycle management.

## Scope

Pure source-grep. Same pattern as
`tests/features/threaded_resize_synchronous`. Pins the exact
regression class that shipped in 0.6.34: 12 keystroke call sites
still had `if (m_pty)` guards from the legacy path; under the
worker path `m_pty` was null, so keystrokes never reached
`ptyWrite()` and nothing echoed.
