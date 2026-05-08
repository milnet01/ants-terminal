# Feature: rewrap preserves wide-char cell pairs across resize

## Problem

`TerminalGrid::resize()`'s `rewrap` lambda at
`src/terminalgrid.cpp:2296-2319` (pre-fix) split logical lines into
fixed-width chunks via `int chunk = std::min(cols, total - pos);`
without inspecting `Cell::isWideChar` / `Cell::isWideCont`. When a
wide-char (CJK ideograph, emoji, fullwidth Latin) sat at the wrap
point, the lead cell stayed on the previous reflowed row while its
continuation cell landed on the next — visual corruption: an
orphaned `isWideChar` cell with no `isWideCont` partner adjacent,
and an orphan `isWideCont` cell with no `isWideChar` lead before it.

Indie-review #3 Lane A M2.

## External anchor

- Unicode Annex #11 (East Asian Width):
  https://www.unicode.org/reports/tr11/ — defines wide vs narrow
  display widths.
- xterm wide-character behaviour: lead + continuation cells must
  remain adjacent on a single visual row; line-wrap moves both
  together.

## Fix

In the chunk loop, if the byte at `pos+chunk-1` is a wide-char lead
AND the chunk doesn't end at total, decrement `chunk` by 1 so the
lead is pushed onto the next iteration's chunk together with its
continuation:

```cpp
if (chunk > 1 && pos + chunk < total &&
    logical.cells[pos + chunk - 1].isWideChar) {
    --chunk;
}
```

The `chunk > 1` guard prevents pathological infinite loops on a
1-cell chunk (which can't be split further). The `pos + chunk < total`
guard skips the decrement when the chunk lands exactly at total
(the wide-char lead at the end is fine — its continuation is also
in this chunk).

## Contract

### Invariant 1 — wide-char pair preserved across rewrap

Construct grid 24×20. Print 10 fullwidth ideographs (`U+4F60` ×
10 = 20 cells). Push the row into scrollback. Resize to 19 cols.
The resized scrollback line should have its wide-chars all paired
(every `isWideChar` followed by an `isWideCont`); no orphan
`isWideCont` should appear at the start of any line.

### Invariant 2 — odd-cols resize doesn't orphan the trailing pair

Construct grid 24×10. Print 5 wide chars (10 cells). Push to
scrollback. Resize to 9 cols. The first reflowed line has 4 wide
chars (8 cells), the second line carries the 5th wide char + its
continuation (2 cells). NEITHER line has an orphan `isWideChar` or
`isWideCont`.

## How this test anchors to reality

Drives the parser end-to-end with literal CJK bytes. Pushes the
wide-char line to scrollback by emitting enough newlines to scroll
it off-screen. Then `resize()` triggers the rewrap path on
`m_scrollback`. Reads back via `scrollbackLine(idx)` and walks each
cell asserting the wide-char lead/cont pairing invariant.

## Regression history

- **Pre-0.7.79:** rewrap chunk loop ignored wide-char boundaries.
  Resizing across a CJK / emoji column count produced visually-
  broken scrollback (cells in apparent wrong positions, wide-chars
  rendered with a space gap or overlapping).
- **0.7.79 (ANTS-1203 from indie-review #3):** chunk loop checks
  `Cell::isWideChar` at `pos+chunk-1` and shrinks by 1 to keep
  the pair together.
