# Inline-image budget + iTerm2 base64 hardening (ANTS-1828 / ANTS-1829)

Two indie-review #6 findings in the TerminalGrid inline-image path.

## Invariants

- **INV-1** (ANTS-1828) `recomputeImageBudget` sums `m_altInlineImages` (the
  saved other-buffer images held across a 1049 alt-screen swap) in addition to
  `m_inlineImages` + `m_kittyImages`. Omitting them let a program breach the
  256 MB per-terminal cap ~2× by filling both the main and alt buffers.
- **INV-2** (ANTS-1829) `handleOscImage` bounds the iTerm2 base64 input BEFORE
  decoding and uses the strict decoder
  (`fromBase64Encoding` + `AbortOnBase64DecodingErrors`), matching the OSC 52 /
  SetUserVar discipline — not the permissive, uncapped `fromBase64`.

## Test

Source-grep conformance against `src/terminalgrid.cpp` (mirrors
`image_bomb_png_header_peek` — a live image test would either pass silently or
hang CI; the regression class is "a future edit drops the guard").
