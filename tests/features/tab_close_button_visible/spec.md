# Feature: Tab close button (×) is always visible and themed

## Problem

User feedback (recurring, 2026-04-25 → 2026-06-11): the per-tab close
(×) glyph is invisible — there's no marker showing where to click to
close a tab.

Two prior attempts failed:

- **0.6.27** removed `image: none` to fall back to the platform's
  standard close icon. Worked on Breeze/Adwaita, but on Fusion / qt6ct /
  some Plasma schemes the platform style rendered the × hover-only.
- **0.7.32 (ANTS-1147)** replaced the fallback with an inline data-URI
  SVG (`image: url("data:image/svg+xml;...")`). **This never rendered.**
  Qt6's stylesheet engine loads `image: url(...)` via `QPixmap(<path>)`,
  which has **no `data:` scheme support** — the glyph drew nothing AND
  suppressed the platform fallback, leaving a blank × on every theme.
  Verified by headless probe 2026-06-11 (see `INVESTIGATION_tab_close_x.md`
  at repo root): data-URI rule → 0 glyph pixels; `image:none` → 0; no
  rule (platform default) → 388; real on-disk `.svg` → 104. The locking
  test was source-grep only, so it stayed green over a dead feature.

## Fix (ANTS-2098)

`ColoredTabBar` draws the close glyph itself on a real, themed
`QToolButton` installed per tab via `QTabBar::setTabButton()` — the only
mechanism verified to both render and re-tint with the theme. Qt's
private `CloseButton` ignores `setIcon()` (also verified: 0 px), so a
settable custom button is required.

- `ColoredTabBar::tabInserted()` installs a `QToolButton` (objectName
  `antsTabClose`) on each new tab's close side, replacing Qt's built-in
  button (which `setTabButton` deletes — no leak).
- The icon is a code-drawn × (two `QPainter` lines) as a `QIcon` with a
  Normal pixmap (theme `textSecondary`) and an Active pixmap (theme
  `textPrimary`) so it re-tints on hover; rendered at the widget's
  device-pixel-ratio for crisp HiDPI strokes.
- Hover background is the theme ansi-red (`ansi[1]`), the will-click cue,
  applied via the button's own QSS.
- `clicked` resolves the button's **live** tab index (tab moves/closes
  reshuffle indices, and Qt keeps the button paired with its tab) and
  emits `tabCloseRequested(index)`.
- `setCloseGlyphColors(normal, hover, hoverBg)` stores the colours and
  re-tints every existing button; wired from `MainWindow::applyTheme`.
- Accessible name `"Close Tab"` is set on the button so AT-SPI / Orca
  keep announcing it (Qt's built-in button carried a translated name;
  see `a11y_chrome_names`).

The dead `QTabBar::close-button` data-URI QSS block and its `%7/%8/%9`
`.arg()` slots are removed from `themedstylesheet.cpp`.

## Contract

This is a **runtime render** test (offscreen `QApplication` from the
`test_chrome` bundle), not a source-grep test — the whole point is to
assert the glyph *renders*, which a grep cannot.

- **INV-1** — every tab carries our themed close button: `tabButton(i,
  closeSide)` is a `QToolButton` with objectName `antsTabClose` and a
  non-empty accessible name.
- **INV-2** — the × renders. Rendering the bar with the glyph coloured to
  match the dark fill (invisible) vs. white yields strictly more white
  pixels when white — i.e. the glyph contributes visible pixels. (Diff
  cancels tab text/borders.)
- **INV-3** — clicking a tab's close button emits `tabCloseRequested`
  with that button's own current tab index.
- **INV-4 (regression)** — `themedstylesheet.cpp` contains no
  `data:image/svg+xml` rule. Reintroducing a data-URI image silently
  re-breaks the glyph (Qt6 QSS can't load it).

## Regression history

- **0.6.27** — platform-style fallback; hover-only on Fusion/qt6ct.
- **0.7.32 (ANTS-1147)** — data-URI SVG; never rendered (Qt6 QSS has no
  `data:` support). Guarded only by a source-grep test that couldn't see
  the runtime failure.
- **ANTS-2098 (2026-06-11)** — themed per-tab `QToolButton` via
  `setTabButton`; this runtime-render test replaces the source-grep one.
