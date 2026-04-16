# Feature contract — combining characters survive terminal resize

## Motivation

A terminal cell holds one base codepoint plus an optional stack of
*combining* codepoints (accents, ZWJ modifiers, skin-tone selectors,
variation selectors). Ants stores combining codepoints in a per-line
side table keyed on column index, separate from the main cell array,
so lines without combiners cost zero extra memory.

When the user resizes the window, `TerminalGrid::resize(rows, cols)`
rebuilds the cell vectors to match the new dimensions. The old
pre-0.6.28 simple-copy path for this code walked cells only and left
the `TermLine::combining` side table in the default-constructed
state. Every resize therefore stripped accents, ZWJ sequences, and
diacritics off of every line — a user with filenames like
`résumé.pdf` or `naïve.cpp` on-screen in a `ls` output would see
them turn into `re?sume?.pdf` and `nai?ve.cpp` the moment they
dragged the window edge.

## Invariants

**I1 — Main-screen combining preservation.**
A `TerminalGrid` that holds a cell at `(row, col)` with a non-empty
combining sequence MUST continue to report that same combining
sequence at `(row, col)` after any `resize()` whose new `cols` is
greater-than-or-equal to the original `col`. Shrink cases where
`col >= new_cols` are out of scope — those cells no longer exist.

**I2 — Alt-screen combining preservation.**
Same as I1 but for the alt-screen buffer (1049 / 47 / 1047 modes).
Alt-screen TUIs (vim, less, htop) render accented filenames and
emoji; losing them on resize is equally user-visible.

**I3 — Combining-map shrink does not crash and does not leak.**
When the new `cols` is *less* than the original, combining entries
whose col is `>= new_cols` MUST be dropped from the resized line.
No out-of-bounds column index may appear in the post-resize
combining map.

## Out of scope

- Widening past the original width does not synthesize combining
  entries for the newly-available columns (those columns are blank).
- Scrollback is treated as immutable; it is not re-copied by resize.
- The reflow path (width-changing main-screen resize that re-wraps
  soft-wrapped lines) is covered by the existing reflow merge logic
  (terminalgrid.cpp — `joinLogical` / `rewrap`), not by this spec.
  This spec targets the no-reflow copy path that the earlier
  feature tests do not exercise.

## Test execution

`test_combining_on_resize.cpp` exercises the feature via the public
`TerminalGrid` / `VtParser` API:

1. Feed `"e\xCC\x81"` (U+0065 LATIN SMALL LETTER E + U+0301 COMBINING
   ACUTE ACCENT) at column 0 — canonical NFD `é`.
2. Assert `screenCombining(0)` contains an entry at column 0 whose
   value is `{0x0301}` — baseline sanity check.
3. Call `resize(rows, cols + 20)` — grow width.
4. Re-assert the entry is still at column 0. This is I1.
5. Enter alt-screen via `ESC [ ? 1049 h`; repeat steps 1-4 using the
   alt-screen code path. This is I2.
6. Call `resize(rows, cols - 5)` where the combining column is within
   the shrink range; assert the entry persists. Then call
   `resize(rows, 2)` where the combining column falls off; assert
   the entry is gone and no out-of-bounds key remains in the map.
   This is I3.

Exit 0 on all invariants holding, non-zero with a diagnostic on any
failure.
