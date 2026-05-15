# sixel_raster_header_prebudget — ANTS-1366

See `docs/specs/ANTS-1366.md`.

## Test scope

Asserts that a Sixel payload with a raster header declaring an
over-cap image dimension is rejected before the parser walks the
full payload, and that small valid Sixels still reach the
renderer.

## Invariants checked

- **INV-1.** Over-cap `Ph` (horizontal) declared via raster
  header is rejected; no image lands in `inlineImages()`.
- **INV-1 (Pv).** Same for over-cap `Pv` (vertical).
- **INV-3.** Small valid Sixel still renders.

## Repro before the fix

Pre-fix, the first-pass walk consumed the entire payload before
the dimension check fired. An over-cap raster header was still
honoured by the cap check, so the image was rejected eventually —
this test asserts the *rejection itself*, not the cost. A
performance-side assertion would be flaky to write portably; the
pre-budget invariant is reject-fast-when-possible, which the test
verifies by ensuring the reject happens before image allocation.
