# Feature: Review Changes button is never visible-but-disabled

## Contract

The status-bar **Review Changes** button (`m_claudeReviewBtn`) MUST be in
exactly one of two states:

| Diff state                    | `isVisible()` | `isEnabled()` |
|-------------------------------|---------------|---------------|
| Clean repo (`git diff` exit 0)| `false`       | (don't care)  |
| Diff present (`git diff` exit 1)| `true`     | `true`        |
| Not a git repo / git error    | `false`       | (don't care)  |

The state `isVisible() == true && isEnabled() == false` MUST NEVER occur
for this button.

## Rationale

The `refreshReviewButton()` policy that decides visibility/enabled-ness
lives at `src/mainwindow.cpp:~3207`. Two interacting facts make the
visible-but-disabled state user-hostile:

1. **Qt swallows clicks on disabled buttons.** `QAbstractButton`'s
   `mousePressEvent` early-returns when the button is disabled, so
   `clicked()` is never emitted. The 0.6.29 reliability pass added a
   `showStatusMessage(...)` call on every silent-return path inside
   `showDiffViewer()` so the user always sees *why* on failure — but those
   guards live INSIDE the click handler. If the click signal never fires,
   the user sees zero feedback.

2. **The global `QPushButton:hover` stylesheet rule
   (`src/mainwindow.cpp:~1852`) is NOT `:enabled`-gated.** A disabled
   button still gets the hover highlight, advertising itself as
   actionable. Combined with the dashed-border disabled styling at
   `~2417-2425` (subtle on dark themes), the user sees what looks like
   a working button that produces no response.

Net symptom (user report 2026-04-17): "*the Review Changes button is
showing and is active as it has an onmouseover event that highlights
the button. When I click the button though, nothing happens.*" Root
cause was `refreshReviewButton`'s clean-repo branch calling
`setEnabled(false); show()` instead of `hide()`.

## Scope

### In scope
- The two valid states above, asserted by direct widget-state inspection.
- Source-level guard against the visible-but-disabled regression in
  `refreshReviewButton` — any future edit that re-introduces
  `setEnabled(false)` adjacent to `show()` on `m_claudeReviewBtn` fails
  the test.
- Source-level check that the global `QPushButton:hover` stylesheet
  rule remains UN-gated by `:enabled` — if that ever changes,
  visible-but-disabled becomes a tolerable state and this spec needs
  to relax. The check is INVERTED: failure means the assumption shifted
  and the spec needs review.

### Out of scope
- The dialog-show contract on click (covered by
  `tests/features/review_changes_click/`).
- The async git-diff-quiet probe itself (uses `QProcess`, which is
  Qt-tested and not the regression vector).
- Theming of the disabled state (not relevant once the disabled state
  isn't visible at all).

## Test strategy

Source-level inspection only — instantiating a `MainWindow` would require
linking `terminalwidget.cpp` + `claudeintegration.cpp` + half the project
just to exercise a 6-line policy block. The contract IS the policy:
"the clean-repo branch must hide, not show-disabled."

The test reads `src/mainwindow.cpp` and asserts:

1. The `refreshReviewButton` function body contains a `btn->hide();` (or
   equivalent) in the `exitCode == 0` branch.
2. The function body does NOT contain `setEnabled(false)` followed (in
   any whitespace) by `show()` on the same button — the regression shape.
3. The global QPushButton stylesheet rule does NOT have a
   `:hover:enabled` form (which would mean the assumption underlying
   this spec changed).

Source-grep matches the same approach used by
`tests/features/review_changes_click/test_review_changes_click.cpp`
and `tests/features/shift_enter_bracketed_paste/test_shift_enter.cpp`
— locks the fix at the binding site without touching widget runtime.
