# Feature: user themes load safely

## Invariants

**INV-1 — a mistyped colour uses the field's fallback.** A user theme whose
`bg_primary` is not a valid colour string loads with `bgPrimary` equal to the
built-in fallback (`#1E1E2E`), not an invalid colour.

**INV-2 — an oversized theme file is skipped.** A `*.json` file larger than
the size cap in `Themes::loadUserThemes` is not loaded.

## Rationale

`parseColor` returned `QColor(string)` for any string, and an invalid `QColor`
paints as black. `loadUserThemes` read each theme file whole on the GUI thread
with no size limit.

## Test surface

`test_user_theme_loading.cpp` points `XDG_CONFIG_HOME` at a `QTemporaryDir`
through `XdgGuard` (with `QStandardPaths` test mode off, which would ignore
it), writes theme files under `ants-terminal/themes/`, and calls
`Themes::loadUserThemes()`.

## Regression history

- **ANTS-5082:** mistyped colours became black and theme files had no size
  cap. Locked by this spec.
