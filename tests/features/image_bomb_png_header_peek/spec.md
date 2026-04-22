# Feature contract — PNG dimension peek before full decode

## Motivation

Inline images reach Ants via two escape-sequence channels:

- **OSC 1337** (iTerm2): base64-encoded image bytes carried inline on
  the wire, dispatched through `handleOscImage` in `src/terminalgrid.cpp`.
- **Kitty graphics APC** (format `f=100` PNG path): base64-encoded
  PNG bytes carried in an APC block, dispatched through `handleKittyApc`.

`QImage::loadFromData()` on either channel performs a *full* decode
before the caller gets a chance to inspect dimensions — so a 1 KB
compressed PNG declaring 100 000 × 100 000 pixels forces ~40 GB of
buffer allocation during the decode, visible as a multi-second freeze
and OOM kill on the host process even though the post-decode guard
(the `MAX_IMAGE_DIM` = 4096 clamp) eventually rejects it.

The 0.7.7 fix introduced a `QImageReader` size peek on the raw byte
stream *before* calling `loadFromData`. `QImageReader::size()` reads
only the PNG/JPEG/etc. header and returns the declared dimensions in
microseconds without allocating decode buffers. If the declared size
exceeds `MAX_IMAGE_DIM`, the byte stream is rejected up front.

Any future change that reverts to a naked `loadFromData(...)` on
untrusted bytes reopens the image-bomb window. This test is the
regression guard.

## Invariants

**I1 — Every `QImage::loadFromData` call on untrusted bytes is
preceded by a `QImageReader`-based dimension peek in the same
function, OR is explicitly tagged `// image-peek-ok` on the same
line to document that the caller has already validated dimensions
through a different mechanism.**

Concretely, for each `loadFromData` call in `src/terminalgrid.cpp`:
- the call line must contain the exact string `// image-peek-ok`, OR
- one of the 10 lines immediately preceding the call must mention
  `QImageReader` (the peek setup).

**I2 — `MAX_IMAGE_DIM` constant is defined and used.** The numeric
clamp (4096 at time of writing) backs the peek — without it the
peek has nothing to compare against.

## Scope

In scope: grep-level source inspection of `src/terminalgrid.cpp`.
This test does *not* exercise the parser at runtime — the dimension
guard is a pre-decode allocator defence; a runtime test that actually
submitted a 100 000 × 100 000 PNG would either succeed (proving the
guard) or hang the test process (proving the regression) and both
outcomes are unsuitable for CI. Static inspection catches the
regression class without those tradeoffs.

Out of scope:
- Decoder bugs in Qt's PNG/JPEG/GIF backends themselves.
- Kitty graphics chunked transfers (`f=24/32` raw RGB/RGBA path
  does not use `loadFromData`).
- iTerm2 image files written through OSC 1337 `File=` attachments
  where the path is a filesystem path (not inline bytes).

## Test execution

`test_image_bomb.cpp` reads `src/terminalgrid.cpp`, enumerates every
line containing `.loadFromData(`, and for each one verifies either
the same-line `// image-peek-ok` marker or a `QImageReader` mention
in the 10 preceding lines. Exit 0 on all matches, non-zero with the
offending line printed on any violation.
