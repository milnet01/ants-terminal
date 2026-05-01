# ClaudeStatusBarController extraction (ANTS-1146)

Companion test for `docs/specs/ANTS-1146.md`. The full
contract — surface-design choice, behavioural-parity statement,
include-hygiene plan, external touch-site rewrite map (12 rows),
per-test re-pointing table for `claude_bg_tasks_button` and
`claude_state_dot_palette` — lives in the spec; this file
documents only what `test_claude_statusbar_extraction.cpp`
itself asserts.

## Invariants pinned by this test

- **INV-1** `src/claudestatuswidgets.h` exists and declares
  `class ClaudeStatusBarController : public QObject` at the
  top level (no namespace). Locks the class identity so a
  future "let's namespace this" PR can't silently relocate it
  and force every connect-PMF to qualify.
- **INV-2a** `src/claudestatuswidgets.h` declares the public
  surface byte-for-byte: 14 method signatures from the spec's
  §1 API (attach, three state pokes, two error helpers,
  resetForTabSwitch, applyTheme, refreshBgTasksButton, four
  provider setters, three accessors).
- **INV-2b** `src/claudestatuswidgets.h` declares the six
  signals byte-for-byte (reviewClicked, bgTasksClicked,
  allowlistRequested, reviewButtonShouldRefresh,
  statusMessageRequested, statusMessageCleared).
- **INV-3** `src/claudestatuswidgets.cpp` contains every
  user-visible label / tooltip byte-identical to the
  pre-extraction code: the 12 "Claude: …" status-label texts,
  the context-bar tooltips, the accessible names, the four
  permanent button labels, the bg-tasks dynamic labels, the
  three permission-group button labels, the `claudeAllowBtn`
  object name, the five tab-indicator tooltip texts.
- **INV-4** `mainwindow.cpp` no longer contains direct
  references to the migrated members:
  `m_claudeStatusLabel`, `m_claudeContextBar`,
  `m_claudeReviewBtn`, `m_claudeErrorLabel`, `m_claudeBgTasks`,
  `m_claudeBgTasksBtn`, `m_claudePromptActive`,
  `m_claudePlanMode`, `m_claudeAuditing`, `m_claudeLastState`,
  `m_claudeLastDetail`. All routed via the controller.
- **INV-5** `mainwindow.cpp` exposes the controller as
  `m_claudeStatusBarController` and contains
  `void MainWindow::setupStatusBarChrome()` (the renamed
  successor to `setupClaudeIntegration`).
- **INV-6** `setupStatusBarChrome`'s body retains the three
  orphans (Roadmap button construct, Update-available
  QAction construct, 5 s startup `QTimer::singleShot`).
- **INV-7** `mainwindow.cpp` contains exactly six
  `connect(m_claudeStatusBarController,` substrings — one
  per signal — and exactly one occurrence of each PMF.
  Stricter than "≥ 1 per signal" — duplicates inside an
  `#ifdef` would slip past a per-signal-existence check.
- **INV-8** Two-sided LoC anchor:
  - `claudestatuswidgets.cpp` LoC ≥ 480.
  - `mainwindow.cpp` LoC strictly decreased vs. parent commit
    by ≥ 480 (verified externally via `git diff --stat`; this
    in-process test asserts only the floor on the new TU).
  - `mainwindow.cpp` no longer defines
    `void MainWindow::updateClaudeThemeColors()` or
    `void MainWindow::applyClaudeStatusLabel()`.
  - `mainwindow.cpp` contains exactly one
    `m_claudeStatusBarController->applyTheme(` substring (the
    central restyle helper at the previous line 3143).

INV-9 (re-pointing the two existing
`claude_bg_tasks_button` / `claude_state_dot_palette` tests)
is verified externally by `ctest -R "claude_bg_tasks|claude_state_dot"`
reporting 2/2 pass post-extraction; not asserted here.
