# Feature: Accessible names on glyph-only chrome controls

ROADMAP item 0.7.x § "Accessibility pass on chrome" (lines 1914–1920):
`mainwindow.cpp` chrome and `CommandPalette` / `TitleBar` buttons
have no `setAccessibleName` / `setAccessibleDescription`. Screen
readers hear "push button" with no label. First pass: one line per
control. Companion item 0.7.x § "AT-SPI introspection lane" (lines
1918–1920) asks for an automated check that every user-visible
control carries an accessible name; this feature test is that
lane.

## Contract

Every chrome control whose visible label is a **non-semantic glyph
or icon** (Unicode arrow, en-dash, X-mark, crosshair, etc.) carries
an explicit `setAccessibleName` and, where the action is non-obvious
without context, a `setAccessibleDescription`. This makes Orca, Speakup,
and AT-SPI introspection tools announce the control by purpose
("Close window") rather than by glyph codepoint ("multiplication
sign x").

Push buttons with **plain English text labels** (`"Background Tasks"`,
`"Review Changes"`, `"Roadmap"`, `"Add"`, `"Edit"`, `"Delete"`, `"OK"`,
`"Cancel"`, …) inherit their accessible name from `QPushButton::text()`
via Qt's `QAccessibleButton` adapter and need no extra wiring — the
test below treats them as already covered.

Tab close buttons are created by Qt (`QTabBar::setTabsClosable(true)`)
and Qt translates their accessible name through the standard "Close
Tab" Qt translation file. Out of scope for this pass.

## Controls and their accessible names

| Control                                | objectName          | accessibleName                  | accessibleDescription                          |
|----------------------------------------|---------------------|---------------------------------|------------------------------------------------|
| TitleBar center-window button (`✥`)    | `centerBtn`         | `Center window`                 | `Center this window on the active screen`      |
| TitleBar minimize button (`–`)         | `minimizeBtn`       | `Minimize window`               | `Minimize this window`                         |
| TitleBar maximize/restore button (`⬜`) | `maximizeBtn`       | `Maximize window`               | `Maximize or restore this window`              |
| TitleBar close button (`✕`)            | `closeBtn`          | `Close window`                  | `Close this window`                            |
| CommandPalette search input            | `commandPaletteInput` | `Command palette search`      | `Type to filter actions; Tab to commit; Esc to dismiss` |
| CommandPalette result list             | `commandPaletteList`| `Command palette results`       | `Available actions matching the current filter` |

Strings are intentionally short and end without a period in the name
field (Orca speaks the period). Descriptions are full sentences.

## Architectural invariants

I1. Each TitleBar button assigns `setAccessibleName` and
    `setAccessibleDescription` immediately after `setText`, before
    `connect`. The strings are literal English in source — no
    `tr()` to keep the AT-SPI test deterministic across LANG values
    (the wider `tr()` migration is the H10 i18n bundle in 0.9.0;
    this pass just unblocks the screen-reader case).

I2. `centerBtn`, `minimizeBtn`, `maximizeBtn`, `closeBtn` each
    carry a stable `objectName` — Qt's title-bar code already sets
    `closeBtn`'s objectName for stylesheet selector purposes; the
    other three get the same treatment for symmetry and so the
    introspection test below has a single way to find them.

I3. `CommandPalette::m_input` already has objectName
    `commandPaletteInput` (line 17). `CommandPalette::m_list`
    already has objectName `commandPaletteList` (line 49). Both
    gain `setAccessibleName` + `setAccessibleDescription` calls in
    the same constructor block.

I4. **Status-bar push buttons** with English labels are out of
    scope: the introspection test asserts coverage via either
    `accessibleName()` non-empty OR `text()` non-empty (Qt promotes
    `text()` to the AT-SPI name when accessibleName is unset).

## Test surface

`tests/features/a11y_chrome_names/test_a11y_chrome_names.cpp`
constructs an offscreen `MainWindow`, walks the chrome widget tree,
and asserts:

T1. `centerBtn` exists, has accessibleName `Center window` and a
    non-empty accessibleDescription.

T2. `minimizeBtn` exists, has accessibleName `Minimize window` and a
    non-empty accessibleDescription.

T3. `maximizeBtn` exists, has accessibleName `Maximize window` and a
    non-empty accessibleDescription.

T4. `closeBtn` exists, has accessibleName `Close window` and a
    non-empty accessibleDescription.

T5. `commandPaletteInput` exists, has accessibleName
    `Command palette search` and a non-empty accessibleDescription.

T6. `commandPaletteList` exists (after the palette has rendered at
    least once) and has accessibleName `Command palette results`.

T7. **Coverage sweep.** Every `QAbstractButton` whose top-level
    parent is the MainWindow's TitleBar OR is reachable from the
    MainWindow's status bar carries either a non-empty
    `accessibleName()` OR a non-empty `text()`. This is the AT-SPI
    introspection lane: it catches a future contributor adding a
    glyph-only button to the chrome without a name.

T8. Source-grep: `titlebar.cpp` has at least four
    `setAccessibleName(` calls (one per chrome button).

T9. Source-grep: `commandpalette.cpp` has at least two
    `setAccessibleName(` calls (input + list).

## Why these strings

"Close window" rather than "Close" because the same widget tree
has a tab close glyph; Orca + JAWS users navigate by accessible
name and need to disambiguate. Same reasoning for "Minimize
window" / "Maximize window" / "Center window."

"Command palette search" pairs with the in-app fuzzy-action
runner; "Command palette results" describes the live-filtered list
beneath it.

## Out of scope (this bundle)

- `tr()` translation hooks for the strings — paired with the
  0.9.0 H10 i18n bundle so the .qm files cover both UI text and
  accessibility strings in one pass.
- Custom `QAccessibleInterface` subclass for `TerminalWidget` —
  bigger H9 a11y bundle, target 0.9.0. The terminal grid itself
  needs a text-changed event lane; that's its own design cycle.
- Status-bar Qt-default-text buttons (`Background Tasks`,
  `Roadmap`, `Review Changes`) — already covered by `text()`-as-
  name promotion. Extending them with explicit accessibleName
  would only add maintenance cost.
