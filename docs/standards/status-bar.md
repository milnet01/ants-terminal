# Ants Terminal status-bar standard

Project-local convention for status-bar widgets in Ants Terminal.
Not part of the shareable `/start-app` standards set — depends on
`MainWindow`-specific architecture (the `m_statusTimer` 2-second
periodic refresh, the startup `singleShot(0)` lambda, and the
`onTabChanged` slot).

## Widget categories

The status bar carries two kinds of widgets:

- **Action** — buttons that the user clicks to do a thing
  (Review Changes, Background Tasks, Roadmap, Audit, Bg-Tasks).
  Visibility is gated on whether the action is currently
  available; click triggers a real operation.

- **State** — read-only labels that report a fact about the
  focused tab or session (git branch, GitHub repo type, error
  banner, Claude context-bar). Visibility is gated on whether
  the fact is true at this moment.

Both categories may read from the focused tab via `shellCwd()`
or other tab-scoped accessors.

## State-category widget refresh contract

Any State-category status-bar widget that reads `shellCwd()`
MUST register on `m_statusTimer` AND the startup `singleShot(0)`
lambda, in addition to any `onTabChanged` connection. The
`onTabChanged` path alone is insufficient because the first call
from `newTab()` runs before `startShell()` sets the PID;
`shellCwd()` then returns empty under `/proc/0/cwd`, the widget
hides itself on first tick, and nothing reschedules it.

Pattern: see `refreshBgTasksButton` (correct — wired to all
three connection points), `refreshRoadmapButton` and
`refreshRepoVisibility` (fixed via ANTS-1160 P2 to follow the
same pattern).

Failure mode this guards against: regression of "widget hidden on
cold launch" bugs (the class first surfaced and was fixed in
v0.6.29). The class shape recurred at v0.7.77 in two widgets
simultaneously (RoadMap button + GitHub repo-type badge) plus a
third architectural cousin (the ANTS-1158 Task List dialog, fixed
via the `claudetasklist.cpp::poll()` mtime-gated rescue).

## Action-category widget refresh contract

Action-category buttons (Review Changes, Background Tasks,
Roadmap-when-shown, etc.) follow the same pattern — they too
read `shellCwd()` to decide visibility, so they get the same
three-connection wiring. The Action vs State distinction matters
for click-handling, not for refresh.

## Adding a new State-category widget — checklist

When adding a new status-bar widget that reads `shellCwd()`:

1. Add a `refresh<WidgetName>()` slot to `MainWindow`.
2. In the ctor, wire it to all three:
   - `connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::refresh<WidgetName>)` — the periodic safety net.
   - `QTimer::singleShot(0, this, [...] { refresh<WidgetName>(); ... })` — the post-startShell first-fire.
   - In `onTabChanged`: `refresh<WidgetName>()` — the immediate response to tab changes.
3. Add a feature-conformance test under `tests/features/` that
   source-greps for all three connections (mirror
   `tests/features/roadmap_status_bar_refresh/`).
4. Document the widget here under "State-category widgets" if
   the catalogue grows large enough to warrant one.

## Related references

- `tests/features/roadmap_status_bar_refresh/spec.md` — the
  feature-conformance test that enforces this contract for the
  RoadMap button and the GitHub repo-type badge.
- `docs/specs/ANTS-1160.md` §9 — the spec section that folded
  the v0.7.77 trio of regressions and added this addendum.
- `src/claudetasklist.cpp::poll()` — the parallel pattern for
  per-tracker refresh rescue (covers the case where
  `QFileSystemWatcher` silently drops its inotify watch on
  atomic-rewrite, which the timer + startup pair does not).
