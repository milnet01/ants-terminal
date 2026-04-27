# Command Palette ghost-text completion

## Surface

The Command Palette (`Ctrl+Shift+P`) displays an inline ghost-text
suggestion drawn from the *top* fuzzy-match in the result list. When
the user has typed a non-empty input that is a **case-insensitive
prefix** of the top match's action text, the unmatched suffix is
rendered in dimmed colour (`palette[Text]` at α=0.45) immediately to
the right of the cursor caret inside the palette's `QLineEdit`. The
visible composition is

```
  ind|ex review              ← `ind` is user-typed, `ex review` is ghost
```

Pressing `Tab` commits the ghost: the input text becomes the full
action name. The user then presses `Enter` (or clicks) to execute —
`Tab` does **not** also execute, matching Claude Code's
`/slash`-completion contract.

If no top match exists, or the top match does *not* start with the
filter (only a `contains` match), the ghost is empty.

The UX scope is intentionally narrow: Command Palette only. The
in-terminal shell ghost-suggestion (fish-style) is a 0.9.0+ design
discussion (`💭` in ROADMAP) — not part of this contract.

## Implementation

A `GhostLineEdit` subclass of `QLineEdit` (declared in
`src/commandpalette.h`) carries the ghost state:

```cpp
class GhostLineEdit : public QLineEdit {
    Q_OBJECT
public:
    void setGhostSuffix(const QString &suffix);
    QString ghostSuffix() const;
protected:
    void paintEvent(QPaintEvent *event) override;
};
```

`paintEvent` calls `QLineEdit::paintEvent(event)` first, then opens a
fresh `QPainter` and draws the ghost suffix at
`cursorRect().right()+1` using `palette().color(QPalette::Text)` with
`setAlphaF(0.45)`.

`CommandPalette::populateList(filter)` calls
`CommandPalette::updateGhostCompletion(filter)` after the list is
built. That helper looks at `m_list->item(0)`, recovers the underlying
`QAction`, strips `&` accelerators, and:

* if the filter is empty, list is empty, or the action name does not
  start with the filter (case-insensitive) → `setGhostSuffix("")`;
* otherwise → `setGhostSuffix(name.mid(filter.length()))` (which
  preserves the action name's original casing in the ghost).

`CommandPalette::commitGhost()` (invoked from `eventFilter` on
`Qt::Key_Tab`) appends `ghostSuffix()` to the current input text via
`setText`. The follow-up `textChanged → filterActions →
populateList → updateGhostCompletion` cycle then clears the ghost
because the new filter equals the action name exactly.

`Tab` is **always** consumed by the palette's event filter
(`return true`) — even when the ghost is empty — so focus cannot leak
out of the input while the palette is open.

`Esc`, `Up`, `Down`, `Enter`, `Return` keep their pre-existing
behaviour. The list-navigation keys do not move the ghost: the ghost
always tracks `m_list->item(0)`, regardless of what is currently
selected.

## Invariants pinned by `test_command_palette_ghost_completion.cpp`

The test runs under `QT_QPA_PLATFORM=offscreen`. It builds a
`CommandPalette`, hands it three `QAction`s with stable text values
(`"Index Review"`, `"Index Browse"`, `"Open File"`), and calls
`show()` to materialise `m_list`.

| # | Invariant |
|---|-----------|
| I1 | `findChild<GhostLineEdit*>("commandPaletteInput")` returns non-null — `m_input` is a `GhostLineEdit`, not a plain `QLineEdit`. |
| I2 | After `m_input->setText("")`, `ghostSuffix()` is empty. |
| I3 | After `m_input->setText("ind")`, `ghostSuffix()` is `"ex Review"` (case preserved from the top action's name; assumes alphabetical iteration order keeps `"Index Review"` ahead of `"Index Browse"` — both tie on prefix score, list takes them in input order). |
| I4 | After `m_input->setText("INDEX")` (uppercase), `ghostSuffix()` is `" Review"` — case-insensitive prefix test, casing preserved from the action name. |
| I5 | After `m_input->setText("File")`, `ghostSuffix()` is empty — `"Open File"` matches via `contains` but does not *start with* `"File"`. |
| I6 | After `m_input->setText("xyz")`, `ghostSuffix()` is empty — no matches at all. |
| I7 | After `m_input->setText("ind")` then sending `QKeyEvent(Tab)` to `m_input`, `m_input->text()` is `"index Review"` (visible composition: user-typed `"ind"` + ghost `"ex Review"`; user casing preserved on commit, mirroring shell-completion semantics) and `ghostSuffix()` is empty. |
| I8 | Sending `QKeyEvent(Tab)` to `m_input` when `ghostSuffix()` is empty leaves `m_input->text()` unchanged and the event is consumed (focus does not move — verified by `m_input->hasFocus()` remaining true). |
| I9 | Source-grep on `src/commandpalette.cpp` — exactly one `setAlphaF(0.45` literal (the dimmed-text color), at least one `cursorRect(` reference (paint position anchor). |
| I10 | Sending `QKeyEvent(Escape)` to `m_input` hides the palette and emits `closed()` — regression guard on existing dismiss behaviour. |

The test verifies the failure mode by stashing the implementation,
rebuilding, expecting non-zero exit, restoring, and confirming the
fixed implementation passes.

## Out of scope

* **Frequency-ranked completion source.** The palette today does not
  track usage frequency; the top match is the first item that passes
  the `contains()` filter. Frequency-ranked top match is a 💭 future
  item.
* **In-terminal shell ghost-suggestion** (fish-style). Different
  surface, different data source, requires OSC 133 prompt-detection +
  history scraping. ROADMAP marks it 💭 with deferral past 1.0.
* **Multi-line ghost / preview pane.** The ghost is a single inline
  suffix only.
* **Mouse-click commit.** Only `Tab` commits the ghost. Clicking
  inside the input edits the input as `QLineEdit` normally does.

## Why this is shippable as a standalone bundle

Implementation is contained to `commandpalette.{h,cpp}` — no other
source touched. The Tab-key contract has no overlap with the
existing key dispatch (the palette had no Tab handler before), and
the `GhostLineEdit` subclass replaces the existing `QLineEdit`
type-by-type, leaving the rest of the palette code unchanged.
