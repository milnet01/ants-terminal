# Feature: Tab close button (×) is always visible, not hover-only

## Problem

User feedback 2026-04-25: "The tabs still don't have a visible
marker per tab that shows where to click to close the tab. The
mouseover works but we need to also see it when onmouseout."

The 0.6.27 fix removed `image: none` from the
`QTabBar::close-button` stylesheet so Qt would fall back to the
platform's standard close icon. That fixed visibility on most
themes, but on Fusion / qt6ct / certain Plasma colour schemes
the platform style still rendered the × hover-only — users
couldn't see where to click without mousing over the tab first.

The fix replaces the platform-style fallback with an explicit
data-URI SVG `image:` rule. The SVG renders an × in the theme's
textSecondary colour (default state) or textPrimary (hover state,
on top of the ansi-red background that already signals
"will-click"). Because the image is baked into the stylesheet,
visibility is independent of platform style — the × always
shows.

## Why a data URI rather than a Qt resource

Themes change at runtime (`themes.cpp`'s 11 themes plus dark/
light auto-switch). Encoding the colour into the SVG lets the
glyph re-tint with the theme without shipping 22 PNG/SVG resource
files. Data URIs are inline so they cost nothing on disk and
nothing per-paint beyond the SVG render Qt would do anyway.

## URL-encoding gotcha

Qt's CSS parser treats `#` as the URI fragment delimiter even
inside `url("...")`, so a literal `#RRGGBB` in the SVG body
truncates the URI at the colour. The encoded form `%23RRGGBB`
is required.

The format string can't carry `%23` literally because
`QString::arg()`'s multi-arg variant processes the format string
once and `%23` would either be misread as a placeholder
reference (if 23 were a valid index, which it isn't here) or
would conflict with neighbouring `%2`/`%3` placeholders. So the
fix splices `QStringLiteral("%23") + colour.name().mid(1)`
into the arg list at the position the SVG's stroke colour gets
substituted from.

## Contract

### Invariant 1 — close-button stylesheet rule contains an explicit image: url(...) data URI

`MainWindow`'s applyTheme stylesheet must contain `QTabBar::close-
button { image: url("data:image/svg+xml;...) }`. The data URI
must contain `<svg ...>` with two `<line>` elements forming an
×.

### Invariant 2 — hover variant exists with its own image rule

`QTabBar::close-button:hover { image: url("data:image/svg+xml;
...") ... }` — distinct from the default-state rule. Hover gives
the user "lifting" feedback (textPrimary stroke + ansi-red
background) when they're aimed at the close button.

### Invariant 3 — URL-encoded `%23` injected via arg-side, not format-side

The applyTheme arg list contains a substring of the form
`QStringLiteral("%23") + ` for the textPrimary and textSecondary
positions, splicing the URL-encoded `#` into the SVG stroke
colour rather than embedding it in the format string. This is
the test that catches a regression where someone "simplifies"
the arg list back to `theme.textSecondary.name()` — that change
would silently truncate the data URI at the colour's `#` and
the × would vanish on themes whose textSecondary doesn't equal
the URL-fragment-default.

### Invariant 4 — image url is independent of platform style

The image rule is set explicitly so QStyle::SP_DockWidgetCloseButton
fallbacks (which are hover-only on some platforms) don't apply.
The `image: url(...)` line MUST be present in BOTH the default-
state rule AND the hover rule.

## How this test anchors to reality

Source-grep on `src/mainwindow.cpp`:

1. `QTabBar::close-button {` and `:hover {` rules each contain
   `image: url(\"data:image/svg+xml;`.
2. Each data URI body contains `<line ... stroke='` so the
   actual × is drawn (not just an empty rectangle).
3. The arg list contains `QStringLiteral("%23")` to lock the
   pre-encoding splice.
4. The hover rule still has `background-color: %7` (ansi-red)
   for the "will-click" cue.

## Regression history

- **First attempt (0.6.27):** removed `image: none` to let Qt
  fall back to platform style. Worked on Breeze/Adwaita; broke
  on Fusion / qt6ct / some Plasma schemes (still hover-only).
- **Reported:** 2026-04-25 — "we need to also see it when
  onmouseout."
- **Fixed:** 0.7.32 — explicit data-URI SVG glyph,
  platform-style independent.
