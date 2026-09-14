# Feature: the About dialog describes the current renderer

## Invariants

**INV-1 — no GPU claim.** `showAboutAnts` in `src/aboutdialogs.cpp` does not
describe the terminal as having GPU rendering.

## Rationale

The OpenGL renderer (`GlRenderer`) was retired in 0.7.44, and `TerminalWidget`
is a `QWidget`; the About text still advertised GPU rendering.

## Test surface

`test_about_text_current.cpp` reads `src/aboutdialogs.cpp`, found beside
`SRC_MAINWINDOW_CPP_PATH`, and checks `showAboutAnts`'s text.

## Regression history

- **ANTS-5082:** the About text named a retired renderer. Locked by this spec.
