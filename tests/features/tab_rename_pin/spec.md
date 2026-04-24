# Feature: right-click "Rename Tab…" pins the label

## Problem

Two code paths can change a tab's label:

- **Right-click → "Rename Tab…"** at `mainwindow.cpp:4274-4291` — user
  action via the tab-bar context menu.
- **rc_protocol `set-title`** via `MainWindow::setTabTitleForRemote`
  at `mainwindow.cpp:2225-2259` — scripted rename over the Unix
  socket.

Both paths need the label to *stick*. The stomp-guards on the
passive rewriters (the per-shell `titleChanged` handler at
`mainwindow.cpp:1660` and the 2 s `updateTabTitles` tick at
`mainwindow.cpp:4008`) both skip tabs present in `m_tabTitlePins`.
`setTabTitleForRemote` populates that map; the right-click rename,
before this fix, only called `setTabText` directly.

Observable consequence: a user renames a tab that has Claude Code
running, and the name reverts within seconds because Claude Code
writes an OSC 0/2 title on every prompt iteration. User report
2026-04-24: *"I renamed a few tabs I had open (some with Claude
Code running) and then Claude Code just renamed them again. Is
this intentional?"*

It was not intentional.

## External anchors

- [xterm ctlseqs — OSC 0 / OSC 2](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html)
  — the title-change escape sequences Claude Code and most modern
  shells emit.
- Ants Terminal rc_protocol `set-title` contract — the reference
  design for how a pinned tab label is supposed to behave.
- `remote_control_set_title/spec.md` (sister feature) — INV-5 and
  INV-6 there describe the pin-map consultation that makes this
  work.

## Contract

### Invariant 1 — rename routes through `setTabTitleForRemote`

The right-click "Rename Tab…" QAction's `editingFinished` lambda
in `mainwindow.cpp` MUST call `setTabTitleForRemote(curIdx, …)`
with the entered text. It MUST NOT call `m_tabWidget->setTabText`
directly — that path skips the pin map.

Grep rule: the lambda captured by the `renameAction`'s
`QAction::triggered` connection contains the substring
`setTabTitleForRemote(` and does NOT contain
`m_tabWidget->setTabText(` within the same handler scope.

### Invariant 2 — empty-name clear path reachable from the rename handler

When the user hits Enter on an empty Rename field, the lambda MUST
still route through `setTabTitleForRemote` (with an empty string)
so the existing pin is cleared and the format-driven /
shell-driven label restored. This is the only in-UI way to undo a
previous rename — without it, once pinned always pinned (until tab
close or an `ants-terminal --remote-title ""`).

Grep rule: the lambda MUST NOT guard the `setTabTitleForRemote`
call with `!newName.isEmpty()`. Empty string is a valid argument
that carries the clear-pin semantics.

### Invariant 3 — `m_tabTitlePins` is the single source of truth

Every tab-label-writing path in `mainwindow.cpp` outside the two
intentional *consumers* (`setTabTitleForRemote` itself, the
format-driven `updateTabTitles`, the OSC 0/2 `titleChanged` signal
handler, `newTabForRemote`'s `"Shell"` initializer, `newTab`
slots, the close path) MUST either (a) consult
`m_tabTitlePins.contains(...)` before writing, or (b) populate
`m_tabTitlePins` when writing. A future rename-like feature that
directly mutates tab text without either check regresses this
contract.

Grep rule: every bare `m_tabWidget->setTabText(` call site in
`mainwindow.cpp` is either (a) inside `setTabTitleForRemote`, (b)
inside `updateTabTitles` (which has already consulted the pin),
(c) inside the `titleChanged` lambda (which has already consulted
the pin), (d) inside `newTabForRemote` with the literal
`"Shell"`, or (e) inside a tab-creation code path where no pin
could yet exist. The right-click rename handler is explicitly
excluded — it must NOT appear on this list because it routes via
`setTabTitleForRemote`.

### Invariant 4 — rename dialog still offers Enter-to-commit UX

Unchanged from pre-fix. The `QLineEdit` editor is created, focused,
and committed on `editingFinished`. This invariant exists so a
future refactor that converts to a `QInputDialog` doesn't silently
drop the contract.

Grep rule: the context-menu action named `"Rename Tab..."` exists,
creates a `QLineEdit`, and connects its `editingFinished` signal.

## Regression history

- **Introduced:** when the right-click Rename action shipped. The
  original implementation pre-dated the pin-map mechanism (which
  landed with rc_protocol `set-title`), and was not revisited when
  the pin map became the canonical guard.
- **Discovered:** 2026-04-24 by the user: "I renamed a few tabs I
  had open (some with Claude Code running) and then Claude Code
  just renamed them again. Is this intentional?"
- **Amplified by:** Claude Code's per-prompt OSC 0 writes. Before
  Claude Code integration the bug existed but was mostly invisible
  because a plain bash prompt sets the title once on startup and
  then rarely — a rename would stick for hours. Claude Code turned
  "rarely" into "every 3–5 seconds", making the bug user-visible.
- **Fixed:** (pending — this slice). One-line change routing the
  rename lambda through `setTabTitleForRemote`.

## What fails without the fix

Without this fix:

1. User opens Claude Code in a tab, renames it to "claude:auth".
2. Claude Code's next prompt iteration writes
   `\e]0;Claude Code - …\a` via its PTY.
3. The `TerminalWidget::titleChanged` signal fires.
4. The handler at `mainwindow.cpp:1652` checks
   `m_tabTitlePins.contains(tabRoot)` — **false**, because the
   right-click rename never populated the map — so it proceeds to
   `m_tabWidget->setTabText(i, "Claude Code - …")`.
5. User's manual rename is lost.

With the fix: step 4 finds the pin, the handler `break`s out
without rewriting, and the manual name survives every OSC 0/2
until the user explicitly clears it via an empty rename.

## Test strategy

`test_tab_rename_pin.cpp` — source-grep test (no GUI bring-up).
Reads `src/mainwindow.cpp` and asserts:

1. The `renameAction`'s triggered lambda body contains
   `setTabTitleForRemote(`.
2. The same lambda does NOT contain
   `m_tabWidget->setTabText(` (the old direct write).
3. The `setTabTitleForRemote` call is NOT wrapped in a
   `!newName.isEmpty()` guard — empty names are valid and carry
   the clear-pin semantics.
4. The existing consumer-side pin guards
   (`m_tabTitlePins.contains` in both the `titleChanged` lambda
   and `updateTabTitles`) are still present — if someone deletes
   them "to clean up," the fix becomes a no-op.

This is the same source-grep pattern as the rc_protocol feature
tests. Rationale: the bug is structural (who writes to which
data structure), not behavioral at the pixel level, and the
existing `remote_control_set_title` test already locks the
behavioral half via `m_tabTitlePins.contains`.

## Verification

Before committing, the test is verified against the pre-fix source
by running it in a worktree that still has the direct
`m_tabWidget->setTabText(curIdx, newName)` call. The test MUST
fail there. Only then is it confirmed to catch the regression
shape.
