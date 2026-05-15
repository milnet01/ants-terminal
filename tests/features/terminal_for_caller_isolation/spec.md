# ANTS-1396 — `terminalForCaller` cross-project fallback isolation

`MainWindow::terminalForCaller(callerCwd)` must NOT fall back to
`focusedTerminal()` when the caller passed an explicit `caller_cwd`
that doesn't match any open Ants tab. v1 fell back unconditionally —
the result was that `get_git_status` and three other tab-anchored
tools (`get_scrollback`, `get_last_command`, `get_environment`)
silently returned data from whatever happened to be the focused
project at the moment, even though the caller's $PWD was a
different project.

Original report (2026-05-15, another CC session): "The MCP
`get_git_status` is showing the Ants Terminal repo (different focused
tab)" — i.e. the caller was running inside project B, requested
git status, and got project A's branch/status/log instead.

## Invariants

- **INV-1 (case 1, back-compat).** Empty `callerCwd` → return
  `focusedTerminal()`. Tools invoked without `caller_cwd` see
  pre-ANTS-1392 behaviour.

- **INV-2 (case 2, match).** Non-empty `callerCwd` that canonicalises
  to a tab's canonical `shellCwd()` → return that tab's terminal.
  First match wins.

- **INV-3 (case 3, no match — the fix).** Non-empty `callerCwd`
  that does NOT match any tab → return `nullptr`. Callers null-check
  and emit an empty/falsy response rather than substituting another
  project's data.

- **INV-4 (case 3, unresolvable cwd).** Non-empty `callerCwd` whose
  `QFileInfo::canonicalFilePath()` is empty (path doesn't exist on
  disk) → treated as case 3; return `nullptr`. Avoids the "did we
  silently degrade to focused?" outcome on a typo.

- **INV-5 (caller contract).** Every caller of `terminalForCaller`
  must null-check the return. Verified at the four invocation
  sites in `mainwindow.cpp`: `get_scrollback`, `get_last_command`,
  `get_git_status`, `get_environment`.

## Test approach

Source-grep — verifies the structural fix is in place. A behavioural
test would need to spin up a `MainWindow` with N tabs whose `shellCwd`
points at distinct test directories, which is overkill for a
3-branch function. Source-grep regression-locks:

- The `case 1` empty-callerCwd back-compat branch survives.
- The `case 3` no-match path returns `nullptr` (literal
  `return nullptr;` present in the function body).
- The function does NOT contain the v1 unconditional
  `return focusedTerminal();` as its last statement.
