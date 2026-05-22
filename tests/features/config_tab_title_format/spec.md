# ANTS-1764 — `tab_title_format` is a fixed enum: validate getter + setter

**Status**: shipped
**Kind**: review-fix
**Source**: indie-review #5 fold-in (2026-05-21), ANTS-1764 bounded slice

## 1. Problem

`Config::tabTitleFormat()` is consumed at `mainwindow.cpp` as a fixed
four-value enum — `"title"`, `"cwd"`, `"process"`, `"cwd-process"`. The
setter (`setTabTitleFormat`) wrote any string through, and the getter
returned whatever was stored (default `"title"`). A zombie value — from a
hand-edit, a forward-version write, or a future enum rename — therefore:

- persists on disk forever (the setter never rejects it), and
- degrades every tab label to the `"Shell"` fallback at runtime (the
  consumer's `if`-chain matches no branch).

This is the cleanly-validatable slice of the broader ANTS-1764 finding
("most `Config` setters don't validate enums"). The remaining keys in that
finding are genuinely *not* config-layer-validatable and stay out of scope:
`theme`/`darkTheme`/`lightTheme` resolve against a dynamic user-theme set
(would need a layering dependency on `themes.cpp`); `quakeHotkey` is a
free-form key sequence; `activeProfile` is a dynamic user-named set. The
three fixed enums that already mirror validation (`roadmapActivePreset`,
`roadmapKindFilters`, `roadmapDensity`) set the ANTS-1179 precedent this
follows.

## 2. Surface

`Config::tabTitleFormat()` / `Config::setTabTitleFormat()` in
`src/config.cpp`. Canonical set is the `QComboBox` data values in
`settingsdialog.cpp:131-134` and the branch labels in
`mainwindow.cpp` `updateTabTitles()`.

## Invariants

- **INV-1** — `tabTitleFormat()` returns one of the four known values;
  any other stored string falls back to `"title"` (mirrors the consumer
  default).
- **INV-2** — `setTabTitleFormat(x)` with `x` outside the known set is a
  no-op: a previously-stored valid value survives unchanged on disk
  (defense-at-write, per ANTS-1179).
- **INV-3** — each of the four valid values round-trips through the setter
  and a fresh `Config` read-back.

## Tests

`test_config_tab_title_format.cpp` in the `test_core` bundle: default,
four-value round-trip, setter rejection of garbage, getter fallback on a
hand-edited on-disk garbage value.
