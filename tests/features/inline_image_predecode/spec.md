# Feature contract — inline images decoded off the GUI thread

ANTS-1061.

## Motivation

`TerminalGrid` applies parsed actions on the GUI thread. Decoding one
4096-pixel PNG there held the window for 100 to 120 ms (measured
2026-10-13, ANTS-1061's roadmap note). `VtStream`'s parse worker now runs
`inlineimage::Predecoder` over every action it parses and ships each
decoded image in `VtBatch::images`. `TerminalWidget::onVtBatch` hands each
one to `TerminalGrid::processAction`, which uses it only when it was
decoded from exactly the base64 text the grid would decode.

## Invariants

- **INV-1.** The predecoder decodes an inline iTerm2 image (OSC 1337
  `File=…inline=1`) and keys it to that action's base64 text.
- **INV-2.** The predecoder follows Kitty chunking: for a PNG sent in
  several frames with its keys on the first only, it returns nothing for
  the `m=1` frames and the decoded image for the final one, keyed to the
  whole base64 text.
- **INV-3.** The predecoder decodes nothing for a Kitty frame that carries
  no PNG to transmit: a placement (`a=p`), a raw-pixel image (`f=24`) or a
  query (`a=q`).
- **INV-4.** Given a matching pre-decoded image, the grid places it without
  decoding the payload itself: an iTerm2 or Kitty payload that is valid
  base64 but not an image still places the pre-decoded image.
- **INV-5.** The grid ignores a pre-decoded image that does not match the
  payload and decodes the payload itself.

## Tests

`test_inline_image_predecode.cpp`, one case per invariant (INV-4 has one
case per protocol). Not covered: that `VtStream` fills `VtBatch::images` and
that `onVtBatch` passes them on. `VtStream` starts a real PTY and has no test
feed, so that wiring is checked by building and by reading it.
