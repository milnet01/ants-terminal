# OSC 8 URI + Kitty APC chunk-buffer memory caps

## Contract

Both `TerminalGrid::processOsc` (OSC 8 hyperlinks) and
`TerminalGrid::handleApc` (Kitty graphics protocol) accumulate
attacker-controlled bytes in long-lived state. The VT parser caps each
OSC / APC envelope at 10 MB, but the downstream accumulators have no
ceiling of their own:

- **OSC 8 URI** (`m_hyperlinkUri`): copied into every `HyperlinkSpan`
  the active hyperlink covers. A 10 MB URI with a one-char anchor copied
  into 50 000 scrollback rows = ~500 GB of RAM. Reachable: hostile
  output over SSH, `cat` of a hostile file, a compromised shell.
- **Kitty APC chunk buffer** (`m_kittyChunkBuffer`): accumulates across
  consecutive `m=1` frames until `m=0` closes the image. An attacker
  who keeps sending `m=1` frames and never closes can grow the buffer
  without bound — the image-budget check only runs on the final frame.

`terminalgrid.h` therefore declares two caps:

- `MAX_OSC8_URI_BYTES = 2048` — real URLs fit comfortably, abuse does
  not. Aligns with common browser/server URL caps (RFC 7230 suggests
  recipients accept at least 8000 octets but implementations commonly
  cap at 2048).
- `MAX_KITTY_CHUNK_BYTES = 32 MiB` — a 24 MiB decoded image is ~32 MiB
  base64-encoded, which is larger than any realistic chunked upload
  (Kitty itself transmits in ~4 KiB frames).

## Invariants tested

- **INV-OSC8-A**: Opening OSC 8 with a URI ≤ cap stores the URI on
  `m_hyperlinkActive` state. Sanity check — the happy path must keep
  working.
- **INV-OSC8-B**: Opening OSC 8 with a URI > cap leaves
  `m_hyperlinkActive = false` and `m_hyperlinkUri` empty. Following
  text prints unlinked (same drop-path as invalid-scheme rejection).
- **INV-OSC8-C**: A large URI does not land in scrollback via a
  printed anchor. After the oversized OSC 8 open, one printed char,
  then OSC 8 close, the screen hyperlink list for that row is empty.
- **INV-APC-A**: A single `m=0` APC frame within the cap decodes into
  the usual path (image rendered or explicitly rejected by the image
  budget — not relevant here; the cap path must not fire). Sanity.
- **INV-APC-B**: Enough `m=1` frames to blow past `MAX_KITTY_CHUNK_BYTES`
  leave `m_kittyChunkBuffer` empty (drain-and-drop), and a follow-up
  `m=0` frame does not resurrect the dropped data.

## Why this test

Two reviewers flagged these accumulators (ROADMAP Cross-cutting themes
and Tier 2 hardening). Both are reachable from unauthenticated
attacker output — the exact DoS primitive that image-bomb defense and
OSC 52 quotas were added to close elsewhere. The test locks the caps
as a behavioral contract so a future refactor that relocates the
accumulator can't silently drop the guard.

Pre-fix behavior: INV-OSC8-B and INV-APC-B fail — the URI is stored at
its full 10 MB length and the chunk buffer grows linearly.

Post-fix behavior: all five invariants hold.
