# Remote-control `tab-list` verb (ANTS-1117 v1)

User request 2026-04-30: surface a richer per-tab snapshot than the
existing `ls` verb — same `index` / `title` / `cwd`, plus the
`shell_pid` / `claude_running` / `color` fields Claude needs to
script across multiple sessions. See `docs/specs/ANTS-1117.md`.

## Surface

- `MainWindow::tabsAsJson()` — produces a `QJsonArray` of one
  object per tab.
- `RemoteControl::cmdTabList()` — dispatches `cmd == "tab-list"`
  and returns `{"ok": true, "tabs": [...]}`.

## Invariants

- **INV-1** `dispatch()` registers a `tab-list` branch alongside
  the existing `ls` branch.
- **INV-2** `cmdTabList` is declared in `remotecontrol.h` and
  defined in `remotecontrol.cpp`.
- **INV-3** `MainWindow::tabsAsJson` is declared in `mainwindow.h`
  and defined in `mainwindow.cpp`.
- **INV-4** Each per-tab object emitted by `tabsAsJson` carries
  the six expected keys: `index`, `title`, `cwd`, `shell_pid`,
  `claude_running`, `color`.
- **INV-5** `claude_running` is computed from
  `ClaudeTabTracker::shellState(pid).state != ClaudeState::NotRunning`
  — the exact same source the status bar consults, no parallel
  detection logic.
- **INV-6** `tab-list`'s response shape mirrors the existing
  `ls` verb's `{"ok": true, "tabs": [...]}` envelope (so
  callers can switch verbs without re-coding the wrapper).
- **INV-7** The narrower `tabListForRemote` (used by `ls`) is
  unchanged — `tab-list` is additive, not replacing.
