# Inline-image budget + iTerm2 base64 hardening (ANTS-1828 / ANTS-1829 / ANTS-2119)

Indie-review findings in the TerminalGrid inline-image paths (#6: budget +
iTerm2 base64; 2026-06-11 terminalgrid M2: the Kitty graphics path).

## Invariants

- **INV-1** (ANTS-1828) `recomputeImageBudget` sums `m_altInlineImages` (the
  saved other-buffer images held across a 1049 alt-screen swap) in addition to
  `m_inlineImages` + `m_kittyImages`. Omitting them let a program breach the
  256 MB per-terminal cap ~2× by filling both the main and alt buffers.
- **INV-2** (ANTS-1829) `handleOscImage` bounds the iTerm2 base64 input BEFORE
  decoding and uses the strict decoder
  (`fromBase64Encoding` + `AbortOnBase64DecodingErrors`), matching the OSC 52 /
  SetUserVar discipline — not the permissive, uncapped `fromBase64`.
- **INV-3** (ANTS-2119 M2) the Kitty graphics path (`handleApc`) strict-decodes
  its base64 too (`fromBase64Encoding` + `AbortOnBase64DecodingErrors`), not the
  non-strict `fromBase64(base64Data)` it used before, which silently skipped
  invalid bytes and fed a garbage-prefixed stream to the image loader /
  raw-pixel `.copy()`. A failed decode leaves the image null → the existing
  ENODATA Kitty-protocol response fires.

## Test

Source-grep conformance against `src/terminalgrid.cpp` (mirrors
`image_bomb_png_header_peek` — a live image test would either pass silently or
hang CI; the regression class is "a future edit drops the guard").
