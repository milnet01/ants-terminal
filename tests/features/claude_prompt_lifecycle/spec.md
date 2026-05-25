# Claude permission-prompt lifecycle — per-shell dedup + tab-switch re-show (ANTS-1850/1851)

Two follow-ups to ANTS-1835 (per-tab permission-prompt routing). The
`permissionRequested` slot and the tab-switch refresh share one anchor
widget (`objectName "claudeAllowBtn"`) that owns the retraction
connections which clear a shell's tab-bar "awaiting input" dot.

## Background

Before this change the slot's dedup deleted **every** `claudeAllowBtn`
anchor on each new prompt (`findChildren -> deleteLater`). Deleting an
anchor severs the `claudePermissionCleared` / `toolFinished` /
`sessionStopped` connections wired to it (they use the widget as context
object). So when two Claude prompts landed in **different** tabs close
together, the second prompt's slot run deleted the first prompt's anchor —
orphaning the connection that would later clear the first tab's dot. The
first tab's "needs attention" dot then stayed lit until a restart even
after its prompt was resolved (ANTS-1850).

ANTS-1835 also suppresses a background tab's bottom-bar message + Allow/
Deny buttons on the focused tab (routing the prompt to the owning tab's
glyph only). The missing half: switching **to** the tab that owns a
still-pending prompt did not re-paint its buttons — only the dot and the
in-terminal prompt remained (ANTS-1851).

The wiring lives in signal/slot lambdas needing a live `QStatusBar`,
PTY-backed `TerminalWidget`s and a focused window, so the GUI structure is
locked by source-scrape (same approach as `claude_statusbar_focus_routing`).
The tracker's rule-retention contract is exercised behaviourally.

## Invariants

- **INV-1 (ANTS-1850 — per-shell dedup)** — `showPermissionPrompt`'s dedup
  loop deletes an existing anchor only when it belongs to the **same**
  owning shell (`claudeAwaitingPid` property == `awaitingPid`) or is a
  property-less scroll-scan button. A **different** shell's pending anchor
  is left intact so its retraction wiring survives to clear its own glyph.
- **INV-2 (ANTS-1850 — pid-tagged anchor)** — every anchor created by
  `showPermissionPrompt` carries
  `setProperty("claudeAwaitingPid", <awaitingPid>)`, so the dedup above can
  attribute it to a shell.
- **INV-3 (ANTS-1850 — owning-terminal scoping)** — the
  `claudePermissionCleared` retraction is wired to the **owning** terminal
  (`term->shellPid() == awaitingPid`), not unconditionally to every
  terminal; the `toolFinished` / `sessionStopped` proxies are guarded by a
  `focusedMatches()` check so the focused tab's tool finishing cannot clear
  a background tab's pending prompt.
- **INV-4 (ANTS-1851 — rule retention)** —
  `ClaudeTabTracker::markShellAwaitingInput(pid, true, rule)` stores `rule`
  in `ShellState::awaitingRule`; `markShellAwaitingInput(pid, false)` clears
  it. The underlying transcript-derived state and the awaitingInput flag's
  emit behaviour are unchanged (rule changes never emit `shellStateChanged`).
- **INV-5 (ANTS-1851 — rebuild on tab switch)** —
  `maybeShowPromptForActiveTab(pid)` re-paints the prompt UI (calls
  `showPermissionPrompt(pid, /*belongsToFocused=*/true, rule)`) iff the
  focused shell's tracker entry has `awaitingInput && !awaitingRule.empty()`;
  `MainWindow::refreshStatusBarForActiveTab` calls it after its Category-C
  anchor teardown.

## Out of scope

- The scroll-scan permission path (`mainwindow.cpp`
  `claudePermissionDetected`) is active-tab-only and never creates a
  background prompt, so it is not re-shown on switch.
- Resolving a backgrounded prompt **while viewing another tab** still can't
  clear that tab's dot via the bottom-bar UI (its anchor was torn down on
  the switch-away); that gap is a single-integration limitation tracked
  separately. INV-3 only guarantees no *wrong*-tab clear.

## Acceptance

`ctest -L features -R ClaudePromptLifecycle` exits zero. Reverting the
per-shell dedup (deleting all anchors), the pid property, the
owning-terminal scoping, the rule retention, or the tab-switch rebuild
each fails the matching invariant.
