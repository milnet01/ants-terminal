# Feature test — `recent_errors` MCP scrollback error extraction (ANTS-1301)

Contract for the `ScrollbackErrors` Core lib + its MCP wiring. Full
design: `docs/specs/ANTS-1301.md`.

## What this test locks

**Live `ScrollbackErrors::parse` behaviour:**

1. `compiler` — GCC/clang `file:line:col?: error:` → category/file/
   line/column/message; column omitted (0) when absent.
2. `lint` — `file:line:col: CODE msg` (ruff/flake8) → category/file/
   line/column, message carries the rule code.
3. `lua` — `lua: file:line: msg`.
4. `test` — ctest `N - name (Failed)` block lines (message = name) and
   `***Failed` markers (message = whole line).
5. `python` — a multi-line traceback collapses to ONE entry; file/line
   from the deepest frame, message from the exception line. Chained
   tracebacks emit one entry each. A frames-only block at end-of-input
   still emits (message = last frame).
6. First-match-wins ordering: a compiler `error:` line is one
   `compiler` entry, never also `lint`.
7. `max_results` keeps the **last** N (newest), sets `truncated`, and
   `errorsTotal` carries the pre-cap count.
8. CRLF input is handled (trailing `\r` stripped); empty/no-match input
   returns an empty result.

**Wiring contract** (source-grep):

9.  `remotecontrol.h` declares `cmdRecentErrors`; `remotecontrol.cpp`
    defines it.
10. `mainwindow.cpp` registers `recent_errors` via
    `registerToolProvider`.
11. `claudeintegration.cpp` carries the tool descriptor, the token-cost
    entry, the `"terminal"` `kindForName` bucket membership, and the
    `TabSpecific` `callerCwdContractFor` branch.

## Invariants

ANTS-5061 — soft-wrapped lines. `cmdRecentErrors` reads `TerminalWidget::recentLogicalOutput`, not
`recentOutput`. A terminal reflows a printed line across several screen
rows once it reaches the pane's right edge; `recentOutput` returns one
row per line, so a parser reading it sees each wrapped fragment as its
own line instead of the one line the shell actually printed. A long
absolute path, or a narrow split pane, makes this common.
`recentLogicalOutput` joins a row the grid marked soft-wrapped to the
row it continues, before the parser ever sees the text; `recentOutput`
itself is unchanged, so `get-text` and the AI dialog keep seeing one
row per line.

- **INV-12** — A compiler-style error line wide enough to soft-wrap,
  where the wrap falls before the word `error:`, is still reported —
  with its file, line, column and full message — once the wrapped rows
  are rejoined. *Test:* `tests/features/mcp_recent_errors/test_mcp_recent_errors.cpp`,
  `McpRecentErrors.SoftWrapJoin`.
- **INV-13** — The same shape wrapping after `error:` instead reports
  the whole message, not the portion before the wrap. *Test:* same.
- **INV-14** — `recentLogicalOutput` returns the wrapped line as a
  single line with no line break inside it; `recentOutput` keeps
  returning one row per line, so its other callers see no change in
  shape. *Test:* same.
- **INV-15** — Two ordinary lines that never reach the pane edge stay
  two separate lines — joining never happens where the terminal didn't
  wrap. *Test:* same.
- **INV-16** — A requested window that starts mid-way through a
  wrapped line has its start moved back to where that line began, so
  the line comes back whole rather than truncated at the window edge.
  *Test:* same.

Exit 0 = every invariant holds.
