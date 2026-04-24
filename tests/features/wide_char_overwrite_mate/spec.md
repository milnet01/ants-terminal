# Wide-char overwrite must zero the mate

## Problem

A wide character (CJK ideograph, wide emoji) occupies two adjacent cells: a
"first half" with `isWideChar=true, codepoint=CP` and a "continuation half"
with `isWideChar=false, isWideCont=true, codepoint=0`. The renderer relies
on the pair staying consistent — `isWideCont=true` means "skip, my neighbor
to the left paints me".

Before this fix, three write paths in `src/terminalgrid.cpp` left stranded
halves when the new write split an existing pair:

1. `handlePrint` narrow write at column `c` — if `cells[c]` was already a
   continuation (`isWideCont=true`), the mate at `c-1` still claimed
   `isWideChar=true`. The wide glyph rendered with a space in the middle,
   or bled one cell to the right on the next redraw.

2. `handlePrint` narrow write at column `c` — if `cells[c+1].isWideCont` was
   true BEFORE we overwrote `cells[c]` (the wide's first half), the
   continuation at `c+1` was orphaned — it rendered as an invisible space
   because its mate was gone, but its `isWideCont` flag still blocked
   selection / copy.

3. `handlePrint` wide write at column `c` (width=2) — same as case 2 but
   against `cells[c+2]`: if the write overwrites a wide first-half at `c+1`,
   the continuation at `c+2` is orphaned.

4. `handleAsciiPrintRun` (the VtParser SIMD fast path for ASCII runs) has
   the same left/right edge issues on its write span `[startCol,
   startCol+span)`.

In all four cases the underlying bug is identical: the write region assumes
its neighbors are well-formed, but wide characters span two cells so the
neighbors on either edge may share a pair with a cell inside the write
region.

## Fix

Add a `TerminalGrid::breakWidePairsAround(row, startCol, endCol)` helper
that runs BEFORE any write:

- Left edge: if `cells[startCol].isWideCont`, the mate at `startCol-1` is
  outside the write region and will strand — clear its `isWideChar`,
  zero its codepoint, drop any combining overlay.
- Right edge: if `endCol < cols && cells[endCol].isWideCont`, its mate at
  `endCol-1` is about to be overwritten from inside the write region —
  clear the orphan's `isWideCont`, zero its codepoint, drop any combining.

Call sites:
- `handlePrint` narrow branch: `breakWidePairsAround(row, col, col+1)`.
- `handlePrint` wide branch: `breakWidePairsAround(row, col, col+2)`.
- `handleAsciiPrintRun`: `breakWidePairsAround(row, startCol,
  startCol+span)` once per row iteration.

## Invariants

- **INV-1**: after a narrow char overwrites the right half of a wide pair,
  the cell at `col-1` no longer claims `isWideChar`.
- **INV-2**: after a narrow char overwrites the left half of a wide pair,
  the cell at `col+1` no longer claims `isWideCont`.
- **INV-3**: after a wide char (width=2) overwrites an existing wide pair
  that starts at `col+1`, the orphaned continuation at `col+2` no longer
  claims `isWideCont`.
- **INV-4**: the fast ASCII path produces the same post-state as the
  character-at-a-time path for wide-char overwrite at either edge.
- **INV-5**: operations that don't overlap any wide pair leave existing
  wide pairs untouched.

## Out of scope

- Resizing: wide chars at the resize seam are already handled by the
  reflow path and the `combining_on_resize` feature test.
- Scroll / erase paths (IL/DL/SU/SD/DCH/ICH): covered by
  `bce_scroll_erase` (0.7.23). The present fix touches only the write
  sites.

## History

- Flagged in the Tier 2 VT/grid correctness sweep (ROADMAP).
- Locked by `tests/features/wide_char_overwrite_mate` which fails 4/5
  subcases against pre-fix `terminalgrid.cpp`, passes all 5 post-fix.
