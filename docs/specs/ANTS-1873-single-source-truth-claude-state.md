# ANTS-1873 — Single-source-of-truth Claude state resolver

**Status:** accepted (2026-05-26) — cold-eyes loops 1–13 folded; converged on polish carry-overs at loop 13.
**Kind:** fix.
**Source:** ROADMAP.md ANTS-1873 (user-report-2026-05-25 + clarifier; see § 1).
**Pairs with:** ANTS-1735 (spec draft) — the autonomous model-switch
gate's INV-2 reads the same per-tab Idle signal; this spec extracts the
helper they both call.

## 1. Problem

The tab-dot indicator (`ColoredTabBar` provider, set up at
`src/claudestatuswidgets.cpp:197-240`) and the bottom status-bar label
(`ClaudeStatusBarController::apply` at
`src/claudestatuswidgets.cpp:1017-1128`) read from **different sources of
truth** for the same conceptual state:

1. **Tab dot** reads `m_tracker->shellState(pid)` — the per-shell
   `ClaudeTabTracker::ShellState` from
   `src/claudetabtracker.h:33-52`. The
   `QFileSystemWatcher`-driven `reparseTranscript` re-derives
   `state` / `tool` / `planMode` / `auditing` on every transcript
   change; the 2 s `pollAllShells` tick
   (`claudetabtracker.cpp:138-147`) only re-detects the Claude child
   PID and triggers a reparse on the binding-transition (Claude
   starts/stops under the shell). Synchronous `markShellAwaitingInput`
   writes the overlay scalars directly.
2. **Status bar** (`apply()` at
   `src/claudestatuswidgets.cpp:1017-1128`) reads cached scalars
   `m_lastState` / `m_lastDetail` / `m_promptActive` / `m_planMode`
   / `m_auditing` (`src/claudestatuswidgets.h:187-191`), populated by
   `ClaudeIntegration::stateChanged`, `planModeChanged`,
   `auditingChanged` signals plus the manual `setPromptActive` /
   `setPlanMode` / `setAuditing` pokes from MainWindow.

**Bidirectional clarifier (user, 2026-05-25):** "The prompting Claude
Code state is fixed for the tab dot but it isn't correct for the status
bar. So, this fix between the tab dot and the Claude Code state in the
status bar needs to be a fix that is bi-directional." — the fix must
reconcile both surfaces against ONE per-tab state source, not patch one
to chase the other.

These three paths drift in both directions:

- **Tab-switch lag.** `resetForTabSwitch()`
  (`src/claudestatuswidgets.cpp:642-646`) blanks every cached scalar to
  `NotRunning` / `false`. The status bar then shows nothing until the
  next `stateChanged` signal arrives. If the newly focused tab's Claude
  is mid-turn, the tab dot correctly shows Thinking (tracker is
  per-tab); the status bar shows nothing or stale — the user-reported
  *"dot shows idle while status bar shows thinking"* (and its inverse
  *"dot shows prompting but status bar doesn't"*).
- **Prompt-state lag.** The PermissionRequest path updates both surfaces
  (tracker via `markShellAwaitingInput`
  `src/mainwindow.cpp:2256,2262,2332,2341`, status bar via
  `setPromptActive` `src/mainwindow.cpp:2260,2276,2334,2343`). But the
  clear-prompt lambda at `src/claudestatuswidgets.cpp:431-438` writes
  `m_promptActive = false` *before* the tracker is updated, so for one
  frame the cached scalar disagrees with the tracker. The status bar's
  read order then surfaces the inverse desync the user saw.
- **Pre-running prompt.** A focused tab whose Claude has been bound
  (`setShellPid`) but no transcript event has landed yet keeps
  `m_lastState == NotRunning`. The current `apply()` short-circuits to
  `hide()` on NotRunning (`src/claudestatuswidgets.cpp:1023-1027`)
  *before* checking `m_promptActive` (`:1039`), so a fast PermissionRequest
  on a freshly-opened tab is invisible on the bar even though the
  tracker entry is correctly flagged. The fix surfaces this overlay
  whenever the tracker says it's pending — see INV-6.

ANTS-1161's `claude_status_bar_per_tab` invariants already gate
cross-tab pollution of `m_state` (foreign session_ids dropped from
`processHookEvent`), so the *singleton* path can't be poisoned by a
sibling tab. What it cannot fix is the **lag** between cached scalars
and the per-tab tracker entry, because the cached scalars exist at all.

The right fix is to delete the parallel cache and read the status-bar
state from the same tracker entry the dot already trusts. One helper
so any caller asking "what's CC doing on the focused tab?" gets the
same answer, by construction.

## 2. Surface

### 2.1 Helper module

New file `src/claudestateresolver.{h,cpp}` in `ants_claude_lib`
(`CMakeLists.txt:340-356` — add to the same
`add_library(ants_claude_lib STATIC …)` list that owns
`claudetabtracker.cpp` at `:349`). The `.h` keeps `TerminalWidget`
forward-declared (the type appears only by pointer in the
`forFocused` signature — no member layout needed); only
`claudestateresolver.cpp` `#include`s `terminalwidget.h` (it calls
`focused->shellPid()`). Consumers (`claudestatuswidgets.cpp`,
future `modelautoswitch` callers) do **not** need to include
`terminalwidget.h` to use `forFocused`.

```cpp
// src/claudestateresolver.h
#pragma once

#include "claudeintegration.h"   // ClaudeState
#include "claudetabtracker.h"    // ClaudeTabTracker::ShellState

#include <QString>
#include <sys/types.h>

class TerminalWidget;

namespace claudestate {

// Display-ready unified Claude state for one Claude session.
// Single source of truth shared by:
//   - the per-tab dot (ColoredTabBar indicator provider),
//   - the bottom status-bar label (ClaudeStatusBarController::apply),
//   - the autonomous switcher gate (ModelAutoSwitch::decide, INV-2).
// Routing both surfaces (and the actuator) through this helper makes
// the agreement structural — they cannot disagree.
struct Resolved {
    ClaudeState base = ClaudeState::NotRunning;
    QString tool;             // non-empty iff base == ToolUse
    bool planMode = false;
    bool auditing = false;
    bool awaitingInput = false;
};

// Lossless adaptation of a tracker ShellState.
Resolved fromShell(const ClaudeTabTracker::ShellState &s);

// Resolve via the tracker, by pid. Returns a default (NotRunning)
// Resolved when tracker is null or pid <= 0.
Resolved forPid(const ClaudeTabTracker *tracker, pid_t shellPid);

// Resolve the focused tab's state via the tracker. Returns a default
// Resolved when tracker is null, focused is null, or shellPid <= 0.
Resolved forFocused(const ClaudeTabTracker *tracker,
                    const TerminalWidget *focused);

// Display-precedence ladder shared by every surface. Order:
//   1. awaitingInput        (most user-actionable signal, can fire
//                            before any transcript event lands)
//   1b. base == ToolUse with tool AskUserQuestion or ExitPlanMode
//                            → AwaitingInput (ANTS-5718)
//   2. planMode             (only when base != NotRunning;
//                            collapses to Hidden otherwise — INV-3)
//   3. auditing             (only when base != NotRunning;
//                            collapses to Hidden otherwise — INV-4)
//   4. base                 (Idle / Thinking / ToolUse / Compacting)
// Returns Hidden when base == NotRunning and no awaitingInput overlay.
enum class Display {
    Hidden,
    AwaitingInput,
    Planning,
    Auditing,
    Idle,
    Thinking,
    ToolUseBash,             // split so dot/label can pick Bash hue
    ToolUseGeneric,
    Compacting,
};
Display display(const Resolved &r);

}  // namespace claudestate
```

The two callers — tab dot and status bar — both consume
`{Resolved, Display}`. The actuator gate (ANTS-1735) reads only
`Resolved.base == Idle` and the existing `composerEmpty` proxy.

### 2.2 Status-bar refactor (apply)

`ClaudeStatusBarController::apply()` rewritten to:

1. Resolve `r = claudestate::forFocused(m_tracker, m_focusedTerminalProvider())`.
2. Compute `d = claudestate::display(r)`.
3. On `Display::Hidden`, hide label + context bar and return.
4. Otherwise pick text and glyph from `d` (Bash/ToolUseGeneric retain
   the tool-name vocabulary mapping at lines 1078-1103).

The tool-vocabulary mapping (read → "reading a file", edit/write →
"editing", grep/glob → "searching", webfetch/websearch → "browsing",
task/agent → "delegating", else raw tool name, at
`src/claudestatuswidgets.cpp:1078-1103`) stays inside `apply()` keyed
on `r.tool` when `d == ToolUseGeneric`. Not extracted to the helper
because the tab dot does not consume it.

The cached members `m_lastState`, `m_lastDetail`, `m_promptActive`,
`m_planMode`, `m_auditing` are deleted. Their setters
(`setPromptActive`, `setPlanMode`, `setAuditing`) become thin
wrappers that simply call `apply()` so existing MainWindow callers
keep working. The setter call sites split into two groups:

**Paired (setter + tracker fire together in the same control flow):**

- **PermissionRequest hook detected** —
  `claudestatuswidgets.cpp:386` tracker `markShellAwaitingInput(.., true)`
  in the `permissionRequested` connect slot; setter `m_promptActive
  = true; apply()` at `:466-467` inside `showPermissionPrompt`'s
  `if (belongsToFocused)` block. (Slot + helper, distinct function
  scopes — the set is split across them by design.)
- **PermissionRequest hook resolved** — `claudestatuswidgets.cpp:431-438`
  `clearPromptActive` lambda, single body: setter `m_promptActive =
  false; apply()` at `:433-434` (wrapped in `if (belongsToFocused)`
  at `:432-435`), tracker `markShellAwaitingInput(.., false)` at
  `:437` (guarded only by `m_tracker && awaitingPid > 0` at `:436`).
  The set/clear sites share the same tracker guard; only the setter
  carries the `belongsToFocused` wrapper. After the refactor the
  setter is gone and `apply()` re-derives from the tracker entry, so
  the `belongsToFocused` wrapper is unnecessary.
- **TUI scroll-scan prompt detected** (`mainwindow.cpp:2256` tracker,
  `:2276` setter) — both in the same outer slot, separated by the
  `clearPromptActive` lambda + button-connect block.
- **TUI scroll-scan prompt resolved** (`mainwindow.cpp:2260` setter,
  `:2262` tracker — fire order) — both inside the `clearPromptActive`
  lambda.
- **Question-prompt detected / cleared** (`mainwindow.cpp:2332/2341`
  tracker, `:2334/2343` setter) — adjacent statements in each slot.

**Unpaired (tab-switch resets with no tracker update):**

- `mainwindow.cpp:5190` `setPromptActive(false)` — fires for **every**
  tab change, *before* the `if (!t)` branch at `:5194`. Harmless
  after the refactor because when `t == nullptr` (last-tab teardown)
  `forFocused(m_tracker, nullptr)` returns the default `Resolved{}`
  per INV-7 → `Display::Hidden` → bar hides; when `t` is non-null,
  the new tab's tracker entry is the source of truth.
- `mainwindow.cpp:5237` `setPlanMode(false)` and `:5238`
  `setAuditing(false)` — both inside the `if (m_claudeStatusBarController)`
  block after `setShellPid(t->shellPid())` at `:5231`, so they only
  run when the new tab exists. Harmless after the refactor because
  the new tab's tracker entry is authoritative for `planMode` /
  `auditing` (kept fresh by the watcher's `reparseTranscript`).

**Provider choice — `m_focusedTerminalProvider`, not
`m_currentTerminalProvider`.** `apply()` paints the bottom-bar
display surface, so it needs the *focused* tab — the same provider
`refreshModelChip` (`claudestatuswidgets.cpp:1140`) and the rest of
the display-paint paths already consume.
`m_currentTerminalProvider` is reserved for handlers that route by
hook session_id first (PermissionRequest slot at `:365`), which is
distinct from "which tab is the user looking at right now". Picking
the wrong provider here would silently re-introduce the per-tab
desync the spec is meant to eliminate.

The `ClaudeIntegration::stateChanged` / `planModeChanged` /
`auditingChanged` connects (lines 285-302) keep their `apply()`
trigger; the body no longer writes the deleted scalars.

### 2.3 Tab-dot refactor

The `setClaudeIndicatorProvider` lambda
(`src/claudestatuswidgets.cpp:197-240`) and its tooltip sibling
(`:248-281`) consume `claudestate::fromShell(s)` then
`claudestate::display(r)` to pick `ClaudeTabIndicator::Glyph`. The
existing precedence (awaitingInput → planMode → auditing → base) is
preserved because the shared `display()` ladder is identical to the
one the lambda hand-coded.

### 2.4 Live status-bar refresh on tracker change

A **new** `connect(m_tracker, &ClaudeTabTracker::shellStateChanged,
…)` is wired inside `ClaudeStatusBarController::attach`, alongside
the existing tab-bar repaint connect at
`claudestatuswidgets.cpp:248-281` (which keeps its tab-bar / tooltip
update body). The new connect's slot calls `apply()` when the changed
pid matches `m_focusedTerminalProvider()->shellPid()`. Without this
the status bar would only refresh on integration signals, leaving
tracker-driven state changes (e.g. `markShellAwaitingInput`,
`reparseTranscript`-driven `planMode` / `auditing` flips) silent on
the bar.

`resetForTabSwitch()` already ends with `apply()`
(`claudestatuswidgets.cpp:665`) — after deleting the cached-scalar
writes at `:642-646`, that trailing `apply()` now re-derives the new
focused tab's display state from its tracker entry the instant the
switch lands, no waiting for the next signal. Widget hides and the
bg-tasks / tasks / model-chip resets at `:647-664` remain; only the
five cached-scalar clears at `:642-646` go.

## 3. Invariants

- **INV-1** — `claudestate::fromShell(s)` returns a `Resolved` whose
  `base/tool/planMode/auditing/awaitingInput` equal `s.state/s.tool/
  s.planMode/s.auditing/s.awaitingInput` (lossless adapter). It also
  carries `s.backgroundShells/backgroundAgents/backgroundMonitors` and
  their sum, `backgroundPending` (ANTS-5773 § 2.3).
  *Test:* `tests/features/claude_state_resolver/`.
- **INV-2** — `display(r)` returns `AwaitingInput` whenever
  `r.awaitingInput` is true, regardless of any other field (the
  user-actionable signal always wins). *Test:* same.
- **INV-3** — `display(r)` returns `Planning` iff `r.planMode &&
  r.base != NotRunning && !r.awaitingInput`. *Test:* same.
- **INV-4** — `display(r)` returns `Auditing` iff `r.auditing &&
  r.base != NotRunning && !r.awaitingInput && !r.planMode`. *Test:*
  same.
- **INV-5** — `display(r)` returns `ToolUseBash` iff `r.base ==
  ToolUse && r.tool.compare("Bash", Qt::CaseInsensitive) == 0`. The
  tab-dot already splits the Bash hue out
  (`claudestatuswidgets.cpp:232`), but does so with a *case-sensitive*
  `==`; the unified contract is **case-insensitive** to match the
  status bar's `toLower()` at `:1077`. All other `ToolUse` tools
  (including empty `tool`) return `ToolUseGeneric`. *Test:* same.
- **INV-6** — `display(r)` returns `Hidden` iff `r.base == NotRunning
  && !r.awaitingInput`. With `r.base == NotRunning` but
  `r.awaitingInput == true`, the result is `AwaitingInput` — an
  **intentional improvement** over the pre-fix `apply()`, which
  short-circuited to hide on NotRunning before checking the prompt
  overlay (see § 1's "Pre-running prompt" sub-case). *Test:* same.
- **INV-7** — `forPid(nullptr, anyPid)`, `forPid(t, 0)`,
  `forPid(t, -1)`, `forFocused(nullptr, anyTerm)`,
  `forFocused(t, nullptr)`, and
  `forFocused(t, term-with-shellPid≤0)` all return the default
  `Resolved{}` — `base == NotRunning`, no overlay set (null-safe).
  *Test:* same.
- **INV-8** — `ClaudeStatusBarController::apply()` reads via
  `claudestate::forFocused` and does NOT reference the deleted
  scalars (`m_lastState`, `m_lastDetail`, `m_promptActive`,
  `m_planMode`, `m_auditing`). *Test:* source-grep against
  `src/claudestatuswidgets.cpp`.
- **INV-9** — The tab-dot indicator lambda
  (`setClaudeIndicatorProvider`) adapts via `claudestate::fromShell`
  and picks its glyph via `claudestate::display`, so both surfaces
  walk the *same* precedence ladder. *Test:* source-grep.
- **INV-10** — `ClaudeStatusBarController` connects
  `m_tracker->shellStateChanged` to a slot that calls `apply()` when
  the changed pid matches `m_focusedTerminalProvider()->shellPid()`
  — so a tracker-driven state change re-paints the bar without
  waiting for an integration signal. *Test:* source-grep over
  `src/claudestatuswidgets.cpp` for the `shellStateChanged` →
  `apply()` pairing within one connect lambda. Live-controller
  behavioural coverage deferred (the source-grep is sufficient to
  lock the wiring).
- **INV-11** — For the **tab dot**, the helper's `display()` is
  semantically equivalent to the pre-fix lambda — including the
  `NotRunning + awaitingInput` case, which the lambda already handled
  by checking `s.awaitingInput` before `s.state`
  (`claudestatuswidgets.cpp:208-216`). For the **status bar**,
  `display()` is equivalent to the pre-fix `apply()` ladder for every
  state EXCEPT the `NotRunning + awaitingInput` case, where the
  pre-fix `apply()` short-circuited to hide before checking the
  overlay (`:1023-1027` before `:1039`) — INV-6 raises this from
  hidden to AwaitingInput, an intentional fix of the "Pre-running
  prompt" sub-case in § 1. *Test:* the existing
  `claude_status_bar_per_tab` / `claude_state_dot_palette` /
  `claude_statusbar_focus_routing` / `claude_status_bar` test suites
  must still pass without modification.

## 4. Tests

- `tests/features/claude_state_resolver/` (label `features;fast`,
  new) — pure behavioural tests of `fromShell`, `forPid`,
  `forFocused`, `display`. Covers INV-1..INV-7 directly + INV-8,
  INV-9, INV-10 via source-grep over
  `src/claudestatuswidgets.cpp`.
- The new test source is appended to `test_claude`'s SOURCES list
  immediately after the `claude_pid_replacement` entry at
  `CMakeLists.txt:982` (the existing `:983` for an unrelated test
  shifts down by one line).
- INV-11 is regression-locked by the existing claude-status test
  suites (they must remain green).
- **Red-gate recipe.** The resolver tests are bundled into
  `test_claude` (no standalone executable per
  `tests/features/README.md`). Verify the failing-stub state by
  running the subset via gtest filter:
  `./build/test_claude --gtest_filter='ClaudeStateResolver.*'`
  against pre-fix stubs of `claudestate::*` (return defaults / wrong
  enum values), confirm RED, then restore the real implementations
  and confirm GREEN.

### Invariant → test function mapping

| INV | Test function |
|---|---|
| INV-1 | `ClaudeStateResolver.Inv1FromShellLossless` |
| INV-2 | `ClaudeStateResolver.Inv2AwaitingInputBeatsEverything` |
| INV-3 | `ClaudeStateResolver.Inv3PlanningRequiresRunning` |
| INV-4 | `ClaudeStateResolver.Inv4AuditingRequiresRunning` |
| INV-5 | `ClaudeStateResolver.Inv5ToolUseBashSplit` |
| INV-6 | `ClaudeStateResolver.Inv6NotRunningHidden` + `Inv6BaseStatesMap` |
| INV-7 | `ClaudeStateResolver.Inv7NullSafe` |
| INV-8 | `ClaudeStateResolver.Inv8ApplyUsesHelper` |
| INV-9 | `ClaudeStateResolver.Inv9TabDotUsesHelper` |
| INV-10 | `ClaudeStateResolver.Inv10ShellStateChangedTriggersApply` |
| INV-11 | existing `test_claude` suites (regression-locked) |

**Layman:** Right now the little circle on the tab and the
"Claude: …" line at the bottom of the window are reading from two
different places, so they sometimes show different things at the same
time. The fix has them both ask the same one place — so they can't
disagree.

## 5. RAM / build cost

- No new state. The helper is pure (free functions, value-type
  struct/enum). Deleting the five cached scalars frees 1 enum + 1
  QString + 3 bools per controller — tiny but net negative.
- One new TU (`claudestateresolver.cpp`) in `ants_claude_lib`. No new
  external library.
- Live update adds one Qt connect (`shellStateChanged` → `apply()`
  via lambda); the slot is cheap (an integer compare against the
  focused pid plus the existing `apply()` body).

## 6. Cross-doc impact

- `docs/subsystems.md` — add `claudestateresolver` entry; amend the
  existing `claudestatuswidgets` entry (`:134-138`) to note the
  status-bar / tab-dot agreement is now structural via the helper.
  (See § Open questions for the separate `claudetabtracker`
  missing-lane gap.)
- `CHANGELOG.md` — on ship.
- No spec rewrites elsewhere; ANTS-1735's INV-2 (focused-tab Idle
  read) is unaffected. The model-switch gate may switch to
  `claudestate::forFocused(m_tracker, focused).base == Idle` for
  clarity when its actuator wiring lands; semantics are identical
  to the current `m_tracker->shellState(...).state == Idle` read, so
  the switch is a cosmetic refactor tracked under ANTS-1735, not a
  separate ROADMAP item.

## Open questions

- **`docs/subsystems.md` missing tracker entry.** `claudetabtracker`
  has no lane in the module map today even though it owns the per-tab
  `ShellState` that drives every Claude indicator. Adding one is
  out of scope for this fix; surface as a separate ROADMAP item
  if useful.
- **Sticky overlay across tab switches.** After the refactor, a prompt
  on tab B remains visible to the status bar if the user switches to
  A and back, because the tracker entry persists per-shell. This is
  arguably the right behaviour (the prompt is still pending) but it
  is a semantic change from today's "reset every overlay on tab
  switch". Calling out as user-observable; no spec change unless the
  user wants the prior behaviour preserved.

## Cold-eyes loop log

Rows live in [`../reviews/ANTS-1873-single-source-truth-claude-state-loop-log.md`](../reviews/ANTS-1873-single-source-truth-claude-state-loop-log.md).
