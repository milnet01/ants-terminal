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

Exit 0 = every invariant holds.
