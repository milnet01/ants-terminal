# Feature: DECRQSS (DCS $ q Pt ST) response shapes

## Contract

DECRQSS — *Request Status String* — is the DEC-defined protocol where
a host application asks the terminal for its current state for a
specific setting. Request is `DCS $ q <Pt> ST`; the terminal replies
with `DCS 1 $ r <Pt> ST` on success, or `DCS 0 $ r ST` if the setting
is not recognized. Apps (neovim, tmux, libvterm-based TUIs, kitty's
kittens) use DECRQSS to save and restore terminal state around their
own output.

The grid's `handleDcs` routes DECRQSS payloads (starting with `$q`)
through a dedicated branch ahead of Sixel detection, and MUST satisfy:

1. **INV-1** — `$qr` (DECSTBM / scroll region request) responds with a
   payload shaped `DCS 1 $ r <top>;<bottom> r ST`. On a freshly
   constructed grid with no scroll region set, top=1 (1-indexed) and
   bottom=`rows`. For a 24-row grid that is exactly `1;24r`.

2. **INV-2** — `$qm` (SGR request) with default `m_currentAttrs`
   responds with `DCS 1 $ r 0 m ST` — i.e. just SGR 0 (reset), no
   attribute flags.

3. **INV-3** — `$qm` after the grid has processed `CSI 1 m` (bold)
   responds with a payload containing `0;1m` — leading reset + bold.
   Combined set (`CSI 1;3;4 m`) produces `0;1;3;4m`.

4. **INV-4** — `$q q` (space + q — DECSCUSR cursor-shape request)
   responds with a payload containing the current cursor-shape digit
   followed by ` q`. On a fresh grid the shape is `BlinkBlock` = 0,
   so the payload contains `0 q`.

5. **INV-5** — Unknown DECRQSS settings (e.g. `$qX`, `$qfoo`) emit the
   invalid-reply shape exactly: `DCS 0 $ r ST` — byte-for-byte
   `\x1BP0$r\x1B\\`.

6. **INV-6** — **Regression guard**: a DCS payload that is a Sixel
   sequence (starts with raster attributes / color index / the `q`
   final byte rather than `$q`) MUST still route to the Sixel handler.
   The DECRQSS branch must not eat Sixel traffic. In particular
   `q#0;2;100;0;0#0` (minimal Sixel stub) must NOT produce a DECRQSS
   reply — no `$r` in the response — and `handleDcs` must return
   without emitting an invalid-reply.

## Rationale

neovim 0.10+ queries DECRQSS SGR at startup to snapshot the user's
pre-neovim state; tmux 3.4+ queries scroll region and cursor shape
similarly. If our response shape is wrong — missing the leading `P1$r`
intro, wrong final byte, bad SGR listing — the apps either hang
waiting for a reply or paint the screen with corrupted defaults.

The Sixel regression guard (INV-6) is the most dangerous one: adding
a DECRQSS branch to `handleDcs` means the payload dispatch is now
bifurcated. A naive `if (payload[0] == '$') { ... } else { sixel }`
is correct, but any refactor that accidentally strips the `$`
requirement (e.g. "handle anything starting with a DCS intermediate")
would silently break Sixel-using apps (`img2sixel`, `timg`,
`chafa --sixel`). This test pins the bifurcation boundary.

## Invariants (operational)

Drive `grid.handleDcs(...)` with a response-capturing lambda wired
via `setResponseCallback`:

- `"$qr"` → capture begins `\x1BP1$r`, ends `r\x1B\\`, and contains
  `1;24r` on a 24-row grid.
- `"$qm"` after reset → capture is exactly `\x1BP1$r0m\x1B\\`.
- `"$qm"` after the parser has seen `CSI 1 m` → capture ends `...0;1m\x1B\\`.
- `"$q q"` (note the space before `q`) → capture contains `0 q` on a
  fresh grid.
- `"$qX"` → capture is exactly `\x1BP0$r\x1B\\`.
- `"q#0;2;100;0;0#0"` (Sixel prefix) → capture does NOT contain
  `$r`, and no invalid-reply `\x1BP0$r\x1B\\` is emitted.

## Scope

### In scope
- The four supported request types (`r`, `m`, ` q`, and invalid).
- The `DCS 1 $ r <Pt> ST` success shape.
- The `DCS 0 $ r ST` invalid-reply shape (exact bytes).
- Sixel dispatch coexistence.

### Out of scope
- DECSCA (`"q` — character-protection attribute): currently unsupported;
  would return invalid-reply, but isn't asserted here because the code
  path treats it as "unknown setting" alongside any other unknown.
- Color DECRQSS (the SGR reply omits color by design — apps querying
  colors use OSC 4/10/11 instead).
- The full Sixel decoder output — INV-6 only checks the *routing*
  decision, not the pixel result.

## Regression history

- **0.7.0** — DECRQSS branch added to `handleDcs` to support neovim's
  background-detect plus tmux's scroll-region save/restore. The
  Sixel regression guard (INV-6) is included preventively: this test
  was written alongside the feature, not after a reported regression,
  because the bifurcation of DCS dispatch is exactly the kind of
  boundary that tends to rot during later refactors.
