# Feature contract — Kitty chunked transmission keeps the first chunk's keys

ANTS-5676.

## Motivation

The Kitty graphics protocol lets a client split one image across several
APC frames. Only the first frame carries the control keys (`a`, `f`, `s`,
`v`, `c`, `r`, `i`); every later frame carries `m=` and optionally `q`
(<https://sw.kovidgoyal.net/kitty/graphics-protocol/>, "Remote client").

`TerminalGrid::handleApc` used to read the keys of the frame it was
handling. On the final `m=0` frame the format fell back to raw RGBA and
the action to transmit-and-display, so a chunked PNG was never shown.

## Invariants

- **INV-1.** A PNG (`f=100`) sent as `a=T` across several frames, with the
  keys on the first frame only, places exactly one inline image whose
  pixel size is the PNG's.
- **INV-2.** A PNG sent as `a=t,i=N` across several frames, with the keys
  on the first frame only, is stored: a later `a=p,i=N` places it.
- **INV-3.** Keys repeated on a later frame still work: when the final
  frame also carries `f=100`, the image is placed as in INV-1.
- **INV-4.** The first frame's keys do not outlive the transmission: a
  single-frame raw image sent after a completed chunked PNG is read with
  its own keys, not the PNG's.
- **INV-5.** Only a continuation frame takes the first frame's keys. A
  frame carrying any key besides `m` and `q` is a command of its own: a
  query (`a=q`) sent mid-transmission, without `m=`, is answered.

## Tests

`test_kitty_chunked_transmission.cpp` drives `TerminalGrid` through
`VtParser`, one case per invariant, and reads `inlineImages()`.
