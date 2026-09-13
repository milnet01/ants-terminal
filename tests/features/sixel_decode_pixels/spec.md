# Feature: a Sixel image decodes to the right pixels

## Invariants

**INV-1 — a full sixel paints all six rows of its column.** `#1;2;100;0;0`
then `~~` produces an image whose columns 0 and 1, rows 0–5, are opaque red.

**INV-2 — a repeat group paints every repeated column.** `#1;2;0;0;100` then
`!3~` produces columns 0–2, rows 0–5, opaque blue. Blue, not green: a red/blue
channel swap in the repeat path leaves green unchanged.

**INV-3 — only the set bits are painted.** `#1;2;0;0;100` then `@` (sixel
value 1) paints row 0 of column 0 opaque blue and leaves row 1 transparent.

**INV-4 — colour passes in one band keep their own columns.**
`#1;2;100;0;0~$#2;2;0;0;100?~` paints column 0 red and column 1 blue: `$`
rewinds, `?` advances without painting.

## Rationale

`TerminalGrid`'s Sixel decoder wrote each pixel with `QImage::setPixelColor`.
A crafted image under the payload cap could reach the decode's column-step
ceiling, so that per-pixel call dominated the GUI thread. The decoder now
converts each colour once and writes through `scanLine`, and stops drawing a
repeat once it passes the image width; the work budget still charges the whole
group (`sixel_repeat_work_cap` INV-4). These invariants pin the decoded pixels
across that change.

## Test surface

`test_sixel_decode_pixels.cpp` feeds a Sixel DCS through `VtParser` into a
`TerminalGrid` and reads `inlineImages().back().image` with `QImage::pixel`.

## Regression history

- **ANTS-5076:** the Sixel decoder's per-pixel `setPixelColor` made a crafted
  image expensive on the GUI thread. Locked by this spec together with
  `sixel_repeat_work_cap`.
