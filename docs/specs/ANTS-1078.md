# ANTS-1078 — Screen-reader accessibility for `TerminalWidget` (H9 core slice)

**Status:** accepted — cold-eyes loops 1–5 folded, converged to
polish/precision (2026-06-29).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-1078 (H9 — AT-SPI/ATK support, 0.9.0 Accessibility); also closes ANTS-2205 (4) (indie-review-8 2026-06-26 LOW cluster — "terminalwidget sets no QAccessible name/role").
**Pairs with:** ANTS-3362 (colourblind-safe themes), ANTS-1079 (high-contrast variants) — sibling a11y items, different surface.

## 1. Problem

The terminal **content area** is invisible to assistive technology. An
earlier bundle (`tests/features/a11y_chrome_names/`) gave the chrome
controls (titlebar / command-palette buttons) accessible names, but it
**explicitly deferred** the terminal grid itself to "the bigger H9 a11y
bundle" (`tests/features/a11y_chrome_names/spec.md:125-127`).

Consequences today:

1. `TerminalWidget` is a plain `QWidget`. With no custom
   `QAccessibleInterface`, Qt's default factory exposes it with role
   `Client` and **no text** — Orca, Speakup, and other AT-SPI clients
   read nothing in the terminal. A blind user hears the window chrome
   but not a single line of output or their own typed command.
2. There is no caret/text-changed signalling, so even an AT that could
   poll has nothing to follow as output arrives.

This blocks the Ubuntu / Fedora accessibility review that distro
packaging gates on (ROADMAP.md, ANTS-1078 bullet — "Without this, Orca
reads nothing in the terminal"), and is a baseline inclusivity gap.

**Layman:** A blind person using a screen reader currently hears
nothing from the terminal's text — this makes the terminal's content
readable, and announces new output as it appears.

## 2. Surface

### 2.1 Scope (this slice vs. full H9)

Delivered here — the "a blind user can read the terminal" core:

- A custom `QAccessibleInterface` for `TerminalWidget` with role
  `QAccessible::Terminal`.
- A `QAccessibleTextInterface` exposing the **visible viewport** as
  plain text, with caret position, word/line boundary navigation, and
  per-character bounding rects.
- A coalesced **text/caret change event** emitted on the existing
  debounced output path, gated on `QAccessible::isActive()` (zero cost
  when no AT is running).

Deferred to later H9 work (§ 5):

- Per-character a11y text **attributes** (colour/bold relayed to the AT).
- Finer-grained `text-inserted` events gated precisely on OSC 133 `D`
  command boundaries (beyond the existing 300 ms output debounce).
- Exposing the terminal **selection** as an AT selection, and
  `setCursorPosition` / `scrollToSubstring` write-back.

### 2.2 Design: thin adapter + content helpers on the widget

The grid-walk primitives `cellAtGlobal` (`terminalwidget.cpp:3109`),
`combiningAt` (`:3168`) and the cell metrics `m_cellWidth` /
`m_cellHeight` / `m_padding` are **private** to `TerminalWidget`.
Rather than make the adapter a friend or expose those internals, the
content logic stays in `TerminalWidget` as a few public helpers, and
the adapter is pure Qt-a11y glue that delegates to them. This keeps the
grid-walk where the data lives and makes the logic unit-testable
without a live AT bridge.

New public methods on `TerminalWidget` (`src/terminalwidget.{h,cpp}`):

```cpp
// Visible viewport as plain text (UTF-16). One line per visible row up
// to the last non-blank row; trailing spaces trimmed per line; trailing
// all-blank rows dropped; rows joined by '\n'. Reflects the current
// scroll position (the frozen snapshot when scrolled back, exactly what
// cellAtGlobal returns). O(rows*cols); built on demand, never cached.
QString accessibleText() const;

// Caret offset within accessibleText(). Derives (row,col) from
// effectiveCursorRow()/effectiveCursorCol() (NOT live m_grid->cursorRow()),
// so the caret tracks the same frozen-or-live source accessibleText()
// shows when scrolled back. A caret column past the trimmed line end
// clamps to that line's end offset. Caret scrolled out of the viewport
// -> characterCount(); blank screen -> 0. Always in [0, characterCount()].
int accessibleCaretOffset() const;

// Widget-relative bounding rect (m_cellWidth x m_cellHeight) of the cell
// holding `offset`. Null QRect for a '\n' separator offset or an
// out-of-range offset.
QRect accessibleRectForOffset(int offset) const;

// Offset of the character under a widget-relative point, or -1 if the
// point is in no cell (padding, past the trimmed line end, blank area).
int accessibleOffsetAt(const QPoint &widgetPos) const;

// One public accessor for the existing inline mapping
// (m_grid->scrollbackSize() - m_scrollOffset), reused by accessibleText().
int firstVisibleGlobalLine() const;
```

These are `const` methods, so — like the cited precedent
`selectedText() const` (`terminalwidget.cpp:3569`, which reads
`m_grid->cols()` at `:3576`) — they read `m_grid` / `cellAtGlobal` /
`combiningAt` / `effectiveCursorRow()` / `effectiveCursorCol()`
**directly**, never the non-const-only accessor `grid()`
(`terminalwidget.h:102` has no `const` overload).
`firstVisibleGlobalLine()` **extracts** the inline mapping currently
duplicated at `terminalwidget.cpp:726` (`scrollbackSize - m_scrollOffset`)
and is called from `accessibleText()` (extract-and-reuse, not a third
copy; existing call-sites may adopt it later).

`accessibleText()` walks all `m_grid->rows()` viewport rows —
`firstVisibleGlobalLine() + r` for `r in [0, m_grid->rows())` — building
each line from `cellAtGlobal(gl, c).codepoint` (treating `0` as space)
plus any `combiningAt(gl, c)` marks — the same per-cell composition
`selectedText()` already uses (`terminalwidget.cpp:3600-3618`).
`cellAtGlobal` is the single source of truth across the scrollback/screen
seam (it already resolves scrollback, frozen-snapshot, and live-screen
ranges), so no special seam handling is needed. Each line then has its
trailing **blank cells** trimmed and trailing all-blank lines dropped
(so an all-blank viewport yields `""`), using the § 2.6 blank-cell
predicate — codepoint-and-combining aware, deliberately distinct from
`selectedText()`'s naive `while (line.endsWith(' '))` chop, so a
trailing space that carries a combining mark is *not* trimmed.
The caret's row/col come from `effectiveCursorRow()` /
`effectiveCursorCol()` (`terminalwidget.h:514-515`), the same
frozen-or-live policy `cellAtGlobal` follows, so the caret never points
into the live screen while the text shows the frozen one. Because
`effectiveCursorRow()` is **screen-relative** (`0..rows-1`) while
`accessibleText()` is indexed by viewport row, the conversion is:
`caretViewportRow = effectiveCursorRow() + m_scrollOffset` (equivalently
`(scrollbackSize() + effectiveCursorRow()) - firstVisibleGlobalLine()`);
the offset is the summed lengths of the emitted lines above
`caretViewportRow` plus the in-line column. If `caretViewportRow` falls
outside `[0, emitted-line-count)` — the caret has been scrolled out of
the viewport — `cursorPosition()` returns `characterCount()` (caret not
in the exposed text). Offsets in the returned string are UTF-16
code-unit indices; the offset↔`(row, col)` mapping accounts for cells
that contribute more than one unit (non-BMP codepoints via
`QString::fromUcs4`, combining marks) — a caret on such a cell maps to
that cell's first UTF-16 unit. `QTextBoundaryFinder` (§2.3) operates on
this same string, so its offsets are in the same unit.

`accessibleRectForOffset` maps the offset back to `(row, col)` and
returns `x = m_padding + col*m_cellWidth`, `y = m_padding + row*m_cellHeight`,
size `m_cellWidth x m_cellHeight` (widget-relative; the adapter maps to
screen via `mapToGlobal`). `accessibleOffsetAt` does the inverse but
**must not** reuse `pixelToCell` (`terminalwidget.cpp:3103-3105`), which
clamps any point into `[0, cols-1] x [0, rows-1]` and so never reports
"no cell"; it applies its own in-bounds test and returns `-1` for a
point in the padding, past a line's trimmed end, or in a dropped blank
area. Edge cases (`'\n'` offsets, wide cells, trailing-space caret,
empty viewport) are defined in § 2.6.

### 2.3 The adapter — `src/terminalaccessible.{h,cpp}`

```cpp
class TerminalWidgetAccessible : public QAccessibleWidget,
                                 public QAccessibleTextInterface {
public:
    explicit TerminalWidgetAccessible(QWidget *w);   // role Terminal, name "Terminal"
    void *interface_cast(QAccessible::InterfaceType t) override;   // -> TextInterface

    // QAccessibleTextInterface
    QString text(int startOffset, int endOffset) const override;
    int characterCount() const override;
    int cursorPosition() const override;
    void setCursorPosition(int) override {}            // read-only in v1
    QString textAtOffset(int offset, QAccessible::TextBoundaryType,
                         int *startOffset, int *endOffset) const override;
    QString textBeforeOffset(int, QAccessible::TextBoundaryType,
                             int *, int *) const override;
    QString textAfterOffset(int, QAccessible::TextBoundaryType,
                            int *, int *) const override;
    QRect characterRect(int offset) const override;
    int offsetAtPoint(const QPoint &point) const override;
    int selectionCount() const override { return 0; }  // v1: no AT selection
    void selection(int, int *startOffset, int *endOffset) const override;
    void addSelection(int, int) override {}
    void removeSelection(int) override {}
    void setSelection(int, int, int) override {}
    void scrollToSubstring(int, int) override {}
    QString attributes(int offset, int *startOffset,
                       int *endOffset) const override; // v1: empty run
private:
    TerminalWidget *term() const;
};
```

- Constructed via the base ctor
  `QAccessibleWidget(w, QAccessible::Terminal, QStringLiteral("Terminal"))`
  so `role()` / `text(Name)` / `state()` / geometry come from the base.
- `interface_cast(t)` returns `static_cast<QAccessibleTextInterface*>(this)`
  for `t == QAccessible::TextInterface` and otherwise delegates to
  `QAccessibleWidget::interface_cast(t)` (so the base's interfaces still
  resolve).
- `text` / `characterCount` / `cursorPosition` delegate to the §2.2
  helpers. `text(s, e)` honours the Qt convention that `e == -1` means
  "to end" (`e = characterCount()`) and clamps `s`, `e` to
  `[0, characterCount()]` before slicing.
- `characterRect(o)` maps the widget-relative `accessibleRectForOffset(o)`
  to screen coordinates as
  `QRect(term()->mapToGlobal(r.topLeft()), r.size())` — `QWidget::mapToGlobal`
  takes a `QPoint` (the `QRect` overload is Qt-6.7+, avoided here); a null
  input rect yields a null output. `offsetAtPoint(p)` =
  `accessibleOffsetAt(term()->mapFromGlobal(p))`. The widget→screen rect
  mapping has no offscreen test surface (no real geometry); it is covered
  by the § 6 manual recipe.
- `textAtOffset` / `textBeforeOffset` / `textAfterOffset` use
  `QTextBoundaryFinder` over `accessibleText()`, handling **every**
  `QAccessible::TextBoundaryType`: `CharBoundary` →
  `QTextBoundaryFinder::Grapheme`, `WordBoundary` → `Word`,
  `SentenceBoundary` → `Sentence`, `LineBoundary` *and*
  `ParagraphBoundary` → `Line` (a terminal row is both), and
  `NoBoundary` → the whole string. All three return their `*startOffset`
  / `*endOffset` clamped to `[0, characterCount()]`.
- `selection(int, int *startOffset, int *endOffset)` must always write
  both out-params; v1 has no AT selection, so it sets
  `*startOffset = *endOffset = -1` (Qt's "no selection" convention) and
  `selectionCount()` returns `0`. The write-side overrides
  (`addSelection` / `removeSelection` / `setSelection` /
  `setCursorPosition` / `scrollToSubstring`) are inert `{}` bodies in v1
  (read-only AT surface; write-back is ANTS-3363).
- `attributes(int offset, int *startOffset, int *endOffset)` returns an
  empty `QString` in v1 (no attributes), with the run clamped to
  `[0, characterCount()]`: `*startOffset = clamp(offset)`,
  `*endOffset = min(*startOffset + 1, characterCount())`, and both `0`
  on an empty buffer. Rich fg/bg/bold attributes are ANTS-3363.

### 2.4 Factory registration

A free function in the adapter header, called from both `main()` and
the feature test (so registration is exercised, not duplicated):

```cpp
void installTerminalAccessibilityFactory();   // QAccessible::installFactory(...)
```

The factory matches `classname == u"TerminalWidget"` and returns a new
`TerminalWidgetAccessible`. Called in `main.cpp` immediately after
`src/main.cpp:164` (the `QApplication app(argc, argv)` ctor line),
before the window is shown.
`TerminalWidget` already has `Q_OBJECT` (it declares signals), so its
metaobject `className()` is `"TerminalWidget"` and the factory covers
every tab created in `MainWindow::createTerminal()`
(`mainwindow.cpp:2097`).

### 2.5 Change events

In the `TerminalWidget` ctor, connect the existing debounced output
signal to a private slot:

```cpp
connect(this, &TerminalWidget::outputReceived,
        this, &TerminalWidget::notifyAccessibilityChanged);
```

`notifyAccessibilityChanged()`:

```cpp
void TerminalWidget::notifyAccessibilityChanged() {
    if (!QAccessible::isActive()) return;          // zero cost when no AT
    QAccessibleTextCursorEvent ev(this, accessibleCaretOffset());
    QAccessible::updateAccessibility(&ev);
}
```

`outputReceived()` is emitted as the first statement of the slot
`checkForClaudePermissionPrompt()` (body at `terminalwidget.cpp:4589`,
`emit` at `:4591`), which is wired to `m_claudeDetectTimer`'s timeout at
`:174`. That timer is a **leading-edge ~300 ms throttle, not a
trailing-edge debounce**: it is started only when idle
(`terminalwidget.cpp:2442-2443`, the ANTS-1862 follow-up comment at
`:2430-2441`), so it fires *during* continuous output (about once per
300 ms) **and** delivers a final trailing scan once output settles. (A
stale comment at `:4792` still mis-labels this timer "trailing-edge"; it
does not affect the design and its doc-fix is tracked by ANTS-3364.) `notifyAccessibilityChanged()` emits at most one
event per invocation (INV-9), so the event rate inherits the throttle's
ceiling — **at most ~1 every 300 ms (~3.3/s)** even under a flood of
output, a cadence AT clients handle routinely. (This ceiling is design
rationale resting on the pre-existing throttle, which has its own tests;
INV-9 covers only the one-event-per-invocation contract this slice owns.)

The caret event prompts a caret-tracking reader (Orca) to re-read the
line at the caret — reliable for the common shell case where the caret
moves down with each new command/prompt. It is *not* a general
new-output announcer: output that streams **above a fixed prompt**
without moving the caret may not be auto-announced (the read path still
exposes it to review commands on demand). ROADMAP ANTS-1078 phrases the
eventual mechanism as `text-changed` / `text-inserted`; v1 deliberately
substitutes a single throttled `QAccessibleTextCursorEvent` (no
old/new-text diff to compute) and defers granular `text-inserted` events
gated on OSC 133 `D` command boundaries — which would close the
fixed-prompt gap — to ANTS-3363 (§ 5). The Orca behaviour is verified by
the manual recipe in § 6.

### 2.6 Offset model and edge cases

The offset unit is the UTF-16 index into `accessibleText()` (what
`QAccessibleTextInterface` and `QTextBoundaryFinder` use). Defined
behaviour for the corner cases an implementer would otherwise guess:

- **`'\n'` separators** — the joins between rows are real characters in
  the string (so `text()` / boundary navigation see line breaks), but
  they map to no cell: `accessibleRectForOffset('\n'-offset)` is a null
  `QRect` and a `'\n'` offset is excluded from the INV-6 round-trip.
- **Blank cell / blank row** — a cell is *blank* iff its `codepoint` is
  `0` or `' '` (`U+0020`) and it carries no combining marks; a row is
  *blank* iff every cell is blank. This is the predicate driving both
  trailing-space trim and trailing-blank-line drop, so a cleared cell
  holding an explicit space counts the same as a never-written `0` cell.
- **Empty / all-blank viewport** — trailing-space trim plus
  trailing-blank-line drop yield `""` for an all-blank screen;
  `characterCount() == 0`, `cursorPosition() == 0`. INV-2's exact-string
  assertion for the blank case is therefore `""`.
- **Maximally scrolled back** — if `m_scrollOffset` ever exceeds
  `scrollbackSize()`, `firstVisibleGlobalLine()` goes negative and the
  top rows resolve to `globalLine < 0`, for which `cellAtGlobal` returns
  a blank cell; those rows render blank and (if trailing) drop. The
  viewport still yields a well-defined string, never an out-of-range
  read.
- **Trailing-space caret** — the terminal caret usually parks one column
  past the last glyph (prompt waiting for input). Since trailing spaces
  are trimmed, a caret column `>=` the trimmed line length clamps to that
  line's end offset (not the untrimmed column, which has no string
  position).
- **Wide / double-width cells** — v1 maps one `m_cellWidth`-wide rect per
  grid column, mirroring `selectedText()` (the base column holds the
  glyph; the `isWideCont` continuation column reads as a space). The
  INV-6 rect round-trip is asserted only over single-column,
  single-UTF16-unit cells; exact wide-glyph extents are ANTS-3363.
- **Scrolled-back viewport** — when scrolled up, `cellAtGlobal` reads the
  frozen screen snapshot (`terminalwidget.cpp:3122-3128`); `accessibleText()`
  inherits this, so the AT view matches the sighted view (the frozen
  content), by design.

## 3. Invariants

- **INV-1** — With an AT active, `QAccessible::queryAccessibleInterface(term)`
  returns a non-null interface whose `role()` is `QAccessible::Terminal`
  and whose `interface_cast(QAccessible::TextInterface)` is non-null.
  *Test:* `terminal_a11y` queries the interface after
  `installTerminalAccessibilityFactory()`.
- **INV-2** — `accessibleText()` is the visible viewport as plain text:
  one line per row in `[0, m_grid->rows())` starting at
  `firstVisibleGlobalLine()`, each line's trailing spaces trimmed,
  trailing all-blank rows dropped, rows joined by `'\n'`; an all-blank
  viewport yields `""`. *Test:* feed known output, assert the exact
  string at scrollOffset 0, when scrolled back, and for a cleared
  (all-blank) screen (`""`).
- **INV-3** — for `0 <= s <= e <= characterCount()`, `text(s,e)` equals
  `accessibleText().mid(s, e-s)`, and `characterCount()` equals
  `accessibleText().size()`. Outside that range `text` first normalises
  per § 2.3 (`e == -1` → `characterCount()`; `s`/`e` clamped) before
  slicing. *Test:* `terminal_a11y` asserts both the in-range equality and
  the `e == -1` / out-of-range normalisation.
- **INV-4** — `cursorPosition()` derives the caret `(row, col)` from
  `effectiveCursorRow()` / `effectiveCursorCol()` (so it follows the same
  frozen-or-live source as `accessibleText()`), equals the caret's offset
  within `accessibleText()` when the caret row is visible, clamps a caret
  column at or past the trimmed line length to that line's end offset,
  and is always in `[0, characterCount()]` (never negative, never
  throws). *Test:* assert with the caret mid-line, in the trimmed
  trailing-space region (the prompt-waiting case), scrolled back (caret
  tracks the frozen snapshot, not the live screen), off-viewport, and on
  a blank screen.
- **INV-5** — `textAtOffset` with `WordBoundary` / `LineBoundary`
  returns the word / line containing `offset` via `QTextBoundaryFinder`,
  with `*startOffset`/`*endOffset` inside `[0, characterCount()]` for
  every offset in range. *Test:* `terminal_a11y` over a multi-word,
  multi-line buffer.
- **INV-6** — for every in-viewport offset whose cell is single-column
  and single-UTF16-unit (excluding `'\n'` separators, trailing-trimmed
  positions, wide cells, and combining sequences — see § 2.6),
  `accessibleRectForOffset(o)` is a `m_cellWidth x m_cellHeight`
  widget-relative rect at that cell and
  `accessibleOffsetAt(accessibleRectForOffset(o).center()) == o`;
  `accessibleRectForOffset` of a `'\n'` or out-of-range offset is a null
  `QRect`; and `accessibleOffsetAt` returns `-1` for a point in the
  padding, past a line's trimmed end, and in a dropped blank-row area.
  *Test:* `terminal_a11y` round-trips the public helpers over a known
  **single-column ASCII** buffer (so offset eligibility is unambiguous —
  no wide/combining cells to exclude) and asserts the three `-1` cases;
  geometry-independent of the AT bridge.
- **INV-7** — `notifyAccessibilityChanged()` does nothing beyond a
  single `QAccessible::isActive()` bool test when no AT is active: no
  interface construction, no text build, no event. *Test:* a `TEST()`
  that reads the slot's source (via the `-DSRC_TERMINALWIDGET_PATH=`
  compile def already present on `test_chrome`, `CMakeLists.txt:1018`)
  and asserts a `QAccessible::isActive()` early-return guard appears
  before any `accessibleCaretOffset(` or `QAccessible::updateAccessibility(`
  token in the slot body (the tokens the slot actually contains —
  matched by token order, not literal first-statement position, robust
  to comments/whitespace). The test extracts and scans **only the
  `notifyAccessibilityChanged` function body** (bounded, not a whole-file
  grep), so it cannot match the unrelated stale-comment region at
  `:4792` (ANTS-3364). The mechanism (a `SRC_*_PATH` compile def) is the
  `scrollback_frozen_view` pattern, whose `test_vt` compile def is at
  `CMakeLists.txt:932`; this test reuses `test_chrome`'s **own**
  `SRC_TERMINALWIDGET_PATH` (`:1018`), not the `test_vt` one.
- **INV-8** — The adapter holds **no** persistent per-widget content
  cache; `accessibleText()` is rebuilt per query, so retained memory is
  O(1) per widget (the interface object, freed with the widget) and
  transient cost is O(rows*cols) only while an AT reads. *Test:* a
  `TEST()` that reads `src/terminalaccessible.h` (via a new
  `-DSRC_TERMINALACCESSIBLE_PATH=` compile def on `test_chrome`) and
  asserts no string/buffer data member (`QString` / `QByteArray` /
  `std::string` / `std::vector`) is declared — matching the
  no-content-cache intent, not just the `QString` case.
- **INV-9** — when an AT is active, `notifyAccessibilityChanged()` emits
  **exactly one** accessibility event per invocation (so the event rate
  inherits the `m_claudeDetectTimer` throttle ceiling, § 2.5). *Test:*
  install a `QAccessible::installUpdateHandler` spy, mark the AT active,
  invoke the slot N times, assert the spy counted N events and that each
  captured event is a `QAccessibleTextCursorEvent` whose
  `cursorPosition()` equals `accessibleCaretOffset()`.

## 4. RAM / build cost

- **Memory:** no persistent content cache. The viewport string is
  transient, O(rows*cols) — a typical 50x200 grid is ~20 KB (10 000
  UTF-16 chars x 2 bytes), built only when an AT queries and freed
  immediately. When no AT is active the feature touches no heap (one
  `isActive()` bool test on the already-throttled output path). The only retained state is one
  `QAccessibleInterface` per live terminal, owned and freed by Qt's
  accessibility cache with the widget. No eviction policy needed (no
  unbounded growth).
- **Build:** one new small TU `src/terminalaccessible.cpp` added to the
  **vt** library (the lib that owns `src/terminalwidget.cpp`,
  `CMakeLists.txt:356`); one new feature-test source
  `tests/features/terminal_a11y/test_terminal_a11y.cpp` added to the
  **`test_chrome`** bundle's `SOURCES` (no new `add_executable`), plus a
  `SRC_TERMINALACCESSIBLE_PATH` compile def on `test_chrome` for the
  INV-8 source-reading test. The test joins `test_chrome`, **not** the
  README's vt-subsystem `test_vt`: constructing a full `TerminalWidget`
  transitively pulls `remotecontrol.cpp` (`ants_core_lib`), which
  references `MainWindow::focusedTerminal()` / `tabsAsJson()` — symbols
  defined only in `ants_chrome_lib`. `test_vt` does not link the chrome
  lib, so a `TerminalWidget`-constructing test fails to link there
  (verified: `undefined symbol: MainWindow::focusedTerminal()`).
  `test_chrome` links the chrome lib (and already hosts the
  `a11y_chrome_names` precedent + the `SRC_TERMINALWIDGET_PATH` def the
  INV-7 test needs). **No new external
  dependency and no new `target_link_libraries` entry needed:**
  `QAccessibleInterface` / `QAccessibleTextInterface` /
  `QAccessibleTextCursorEvent` are in **QtGui**, `QTextBoundaryFinder` is
  in **QtCore**, and the adapter base `QAccessibleWidget` is in
  **QtWidgets** — all reached transitively by the vt lib's existing
  `Qt6::OpenGLWidgets` link (`CMakeLists.txt:360`, which pulls
  `Qt6::Widgets` → `Qt6::Gui` → `Qt6::Core`); the vt lib does not list
  `Qt6::Widgets` explicitly. `QAccessible::Terminal` is a Qt6 role
  (verified present in Qt 6.11).

## 5. Out of scope

- Per-character a11y **text attributes** (colour/bold) — `attributes()`
  returns an empty run in v1; tracked by ANTS-3363.
- OSC 133 `D`-gated `text-inserted` granularity beyond the ~300 ms
  output throttle — ANTS-3363.
- Exposing the terminal **selection** as an AT selection and
  `setCursorPosition` / `scrollToSubstring` write-back — ANTS-3363.
- Exact wide / double-width-cell glyph rects in `characterRect`
  (v1 maps one rect per grid column) — ANTS-3363.
- High-contrast / colourblind theme variants — ANTS-1079 / ANTS-3362.

## 6. Tests

Feature test: `tests/features/terminal_a11y/` (`spec.md` +
`test_terminal_a11y.cpp`), GoogleTest `TEST(TerminalA11y, …)` blocks
using the shared `tests/_support/expect.h` helper (per
`tests/features/README.md`), added to the **`test_chrome`** bundle's
`SOURCES`; ctest label `features;fast` (inherited from the `test_chrome`
bundle, `CMakeLists.txt:797`). Covers INV-1..INV-9.

Runs under `QT_QPA_PLATFORM=offscreen`: constructs a `TerminalWidget`,
feeds synthetic output into `widget->grid()` via `VtParser` (the
widget's own parse path), then:

- INV-2/4/6 — asserts the public `accessible*()` helpers directly
  (no AT bridge needed; geometry-independent).
- INV-1/3/5 — calls `installTerminalAccessibilityFactory()` and
  `QAccessible::setActive(true)` (the offscreen platform does not load an
  AT plugin on its own), then asserts `QAccessible::queryAccessibleInterface(widget)`
  returns the interface with `role()==QAccessible::Terminal` and a
  non-null `textInterface()`, and exercises `text` / `characterCount` /
  `cursorPosition` / `textAtOffset`.
- INV-7/INV-8 — source-reading `TEST()`s that open the slot / adapter
  source via the existing `SRC_TERMINALWIDGET_PATH` and a new
  `SRC_TERMINALACCESSIBLE_PATH` compile def (added to `test_chrome` by
  this slice — see § 4) — the `scrollback_frozen_view` pattern.
- INV-9 — `QAccessible::installUpdateHandler` spy counts one event per
  `notifyAccessibilityChanged()` call under an active AT. Activation is
  Qt-version dependent — `QAccessible::setActive(true)` only flips
  `isActive()` on Qt 6.8+ (where `setActive()` forwards to the platform
  accessibility object); older distro Qt (e.g. CI's 6.4 under offscreen)
  leaves `isActive()` false, so the test asserts the one-event-per-call
  contract when `isActive()` is true and the zero-events guard when it is
  false. (The ~3.3 events/s flood *ceiling* is not re-tested here: it rests
  on the pre-existing `m_claudeDetectTimer` throttle, which has its own
  tests; this slice owns only the one-event-per-invocation contract.)

Verify each assertion fails against pre-implementation source before
wiring the implementation.

**Manual recipe (not in CI).** Run Ants under Orca on a Linux desktop:
confirm Orca announces terminal lines on focus and read-cursor review,
follows the caret as commands are typed/run, and reads the prompt line.
Note the known v1 limitation (§ 2.5): output streaming above a fixed
prompt may not auto-announce until the caret moves — closed by ANTS-3363.

## 7. Cross-doc impact

- **CHANGELOG.md** — new "Added" entry (terminal screen-reader support).
- **ROADMAP.md** — ANTS-1078 flipped to 🚧 then annotated done for this
  slice (remaining refinements noted); ANTS-2205 (4) annotated resolved.
- **CLAUDE.md** — module map: note `src/terminalaccessible.cpp` if the
  subsystem catalogue lists per-file lanes (otherwise no change).
- No README / PLUGINS.md change (no user config key, no Lua surface).

## Cold-eyes loop log

**Loop 1 (2026-06-29)** — two independent cold reviewers. Verified
findings fixed: **1 CRITICAL** — §2.5 mischaracterised `m_claudeDetectTimer`
as a trailing-edge debounce ("fires only after output settles");
verified at `terminalwidget.cpp:2430-2443` it is a *leading-edge ~300 ms
throttle* that fires during continuous output, so §2.5 + INV-7 were
rewritten around the real cadence (≤~3.3 events/s ceiling).
**2 HIGH** — `accessibleText() const` called the non-const-only `grid()`
(`terminalwidget.h:102`); rewritten to read `m_grid` directly like
`selectedText()` (H1). Wide/double-width-cell round-trip undefined;
scoped in § 2.6 + INV-6 (H2). **3 MEDIUM** — unnamed test bundle / label
/ grep-lane mechanism (fixed: `test_chrome` bundle, GoogleTest model,
source-reading `TEST()`s); `'\n'`-offset rect undefined (§ 2.6);
empty/all-blank viewport string undefined (§ 2.6 + INV-2);
trailing-space caret offset undefined (§ 2.6 + INV-4); `selection()`
out-param contract unstated (§ 2.3). **LOW** — deferred-work now cites a
real id (ANTS-3363); Qt-version note for the `Terminal` role;
caret-event-vs-`text-changed` substitution noted; ROADMAP cite made
drift-proof. Dismissed (unverified): reviewer-A INFO that the ROADMAP
gate citation was off by 2 lines — it was correct.

**Loop 2 (2026-06-29)** — two fresh cold reviewers; no loop-1 finding
resurfaced (fixes held). Verified findings fixed: **1 CRITICAL** —
`accessibleCaretOffset()` didn't use the existing
`effectiveCursorRow()/effectiveCursorCol()` frozen-vs-live policy
(`terminalwidget.h:514-515`), so the caret would desync from the frozen
viewport text when scrolled back; now pinned to those accessors in
§ 2.2 + INV-4 (with a scrolled-back assertion). **HIGH** — the
scrollback/screen seam in `accessibleText()` left unstated (now: always
`rows` lines, `cellAtGlobal` the single source across the seam);
`SRC_TERMINALACCESSIBLE_PATH` framed inconsistently as new-vs-present
(now marked added-by-this-slice). **MEDIUM** — ctest label was
`features` but the standard + the `test_chrome` bundle use `features;fast`
(`CMakeLists.txt:797`); the no-flood rate ceiling had no test surface
(added INV-9, one-event-per-invocation via a `QAccessible` spy, and
demoted the ~3.3/s figure to rationale); `firstVisibleGlobalLine()`
extract-vs-duplicate ambiguity (now: extracts the `:726` inline
mapping); blank cell/row predicate undefined (now defined in § 2.6 as
codepoint `∈ {0, ' '}`, no combining); `attributes()` could return
`*endOffset` past `characterCount()` on a blank buffer (now clamped).
**LOW** — RAM figure corrected to ~20 KB (UTF-16); INV-7 grep reworded
to token-order (not literal first-statement); § 2.5 reworded to name the
`checkForClaudePermissionPrompt()` timeout slot; honest scoping of the
caret-event limitation for output streaming above a fixed prompt
(manual Orca recipe added to § 6, deferred close to ANTS-3363).

**Loop 3 (2026-06-29)** — two fresh cold reviewers; no loop-1/2 finding
resurfaced. Verified findings fixed: **HIGH** — the caret screen-row→
viewport-offset arithmetic was unstated (now § 2.2:
`caretViewportRow = effectiveCursorRow() + m_scrollOffset`); the
off-viewport caret offset was undefined (now `characterCount()` with its
trigger named); INV-7's grep token was *vacuous* — it matched
`accessibleText(`, which the slot never calls (now matches
`accessibleCaretOffset(` / `QAccessible::updateAccessibility(`); § 4
mis-stated the link surface — `QAccessibleWidget` is QtWidgets (not
QtGui), reached transitively via the vt lib's `Qt6::OpenGLWidgets`
(`CMakeLists.txt:360`), no explicit `Qt6::Widgets` (now corrected).
**MEDIUM** — the `scrollback_frozen_view` precedent is in `test_vt`, not
`test_chrome` (clarified: mechanism reused via `test_chrome`'s own
`SRC_TERMINALWIDGET_PATH`); blank-cell trim is combining-aware and
deliberately differs from `selectedText()`'s naive space-chop (stated);
`accessibleOffsetAt` must not reuse `pixelToCell`'s clamp, which never
returns "no cell" (stated); the flood ceiling rests on the throttle's
own tests, not this slice (noted in § 6). **LOW** — INV-8 now asserts no
string/buffer member (not just `QString`); two off-by-one citations
tightened (`a11y_chrome_names/spec.md:125-127`; `main.cpp:164` is the
ctor line); composition-loop cite `:3600-3618`; non-BMP caret maps to
the cell's first UTF-16 unit.

**Loop 4 (2026-06-29)** — two fresh cold reviewers; no prior-loop finding
resurfaced. Verified findings fixed: **HIGH** — §2.3 sketched
`mapToGlobal(accessibleRectForOffset(o))`, but `QWidget::mapToGlobal`
takes a `QPoint` (the `QRect` overload is Qt-6.7+); now
`QRect(mapToGlobal(r.topLeft()), r.size())`, with the widget→screen
mapping noted as manual-only (no offscreen geometry). §4 mis-filed
`QTextBoundaryFinder` as QtGui — it is **QtCore** (corrected; conclusion
unchanged, still transitively linked). **MEDIUM** — `textAtOffset` &
friends now enumerate *every* `QAccessible::TextBoundaryType`
(`CharBoundary`→Grapheme, Word, Sentence, Line+Paragraph→Line,
`NoBoundary`→whole) rather than an incomplete list missing
`CharBoundary`/`NoBoundary`; INV-6 now pins its buffer to known
single-column ASCII and adds the three `accessibleOffsetAt → -1`
no-cell assertions; §4 now justifies `test_chrome` over the
README-implied `test_vt`. **LOW/INFO** — `text(s, e)` honours the
`e == -1`-means-end Qt convention; `interface_cast` delegates non-Text
types to the base. Also surfaced (code-side, not a spec defect, now
roadmapped): a stale comment at `terminalwidget.cpp:4792` still calls
`m_claudeDetectTimer` a "single-shot trailing-edge" timer — contradicts
the authoritative ANTS-1862 leading-edge-throttle comment at `:2430-2443`
this spec relies on.

**Loop 5 (2026-06-29)** — two fresh cold reviewers; both independently
flagged the **same single substantive item** (the loop-3-introduced
contradiction), and nothing structural/architectural surfaced. Verified
findings fixed: **HIGH** — §2.2 said `accessibleText()` "always yields
exactly `rows` lines" while §2.6/INV-2 drop trailing all-blank lines;
reworded so the walk (all rows) and the trailing-blank trim are
sequential, not both "always." INV-3's `text(s,e) == mid(s,e-s)`
equality is only unconditional in range; now scoped to
`0 <= s <= e <= characterCount()` with the `e == -1`/clamp normalisation
called out separately. **MEDIUM/precision** — INV-9 now also asserts the
emitted event is a `QAccessibleTextCursorEvent` carrying
`accessibleCaretOffset()` (not just an event count); INV-7's source scan
is bounded to the `notifyAccessibilityChanged` function body so it can't
match the `:4792` stale region; INV-8 widened to any string/buffer
member; the `test_vt` compile-def cite corrected `:928`→`:932` (`:928` is
the comment line); §2.5's "`:174` timeout slot" corrected (`:174` is the
`connect`; slot body at `:4589`), and the stale `:4792` comment now cited
as tracked by ANTS-3364. **Dismissed (unverified):** a reviewer also
named `terminalwidget.cpp:179` as a third stale timer comment — that line
is `m_syncTimer.setInterval(500)`, unrelated; ANTS-3364 widened to the
genuine second site `terminalwidget.h:123` only.

**Convergence / sign-off (2026-06-29).** Five loops, ten independent cold
reviews, zero CRITICAL after loop 2 and zero structural/architectural
findings after loop 2. The tail (loops 3–5) found only
internal-consistency, invariant-precision, Qt-API/module accuracy, and
citation items — all fixed. Per the project's polish-convergence rule
(a loop whose only verified findings are polish/precision, nothing
structural/mechanical/architectural, is the sign-off gate), the spec is
**accepted**; ready to implement.
