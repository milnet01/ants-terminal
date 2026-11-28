# Feature contract — smaller terminal-grid fixes from the 2026-11-06 reviews

ANTS-6653. Each invariant is one row of
`docs/reviews/close-findings-2026-11-06.md` (§ Other bugs); the row number
follows the invariant.

## Invariants

- **INV-1** (row 10). A CSI carrying a private marker or intermediate acts
  only where the grid handles that marker: `CSI ? Pi;Pa;Pv S` does not
  scroll and `CSI ? Pm r` does not set the scroll region. `CSI ? J` and
  `CSI ? K` still erase, as ED and EL do.
- **INV-2** (row 11). A DCS is Sixel only when it opens `[0-9;]*q`;
  XTGETTCAP (`DCS + q … ST`) places no image.
- **INV-3** (row 12). A Kitty graphics command gets a reply only when it
  carries `i=` or `I=`. `q=1` suppresses the OK reply; `q=2` suppresses
  the error reply too.
- **INV-4** (row 15). Entering the alternate screen leaves no cell marked
  wide or wide-continuation, and no row marked soft-wrapped.
- **INV-5** (row 16). On a width-change reflow, blank rows at the bottom
  are dropped before any content row is pushed into scrollback.
- **INV-6** (row 20). DCH and ICH move OSC 8 links with their text and drop
  the link the edit splits. DCH breaks a wide pair one of whose halves it
  deletes.
- **INV-7** (row 22). A truncated OSC 1337 payload is refused, as OSC 8
  and OSC 52 are.
- **INV-8** (row 23). A prompt region whose output starts on line 0 reports
  its output from line 0.
- **INV-9** (row 24). Kitty `a=d`: `d=a` and `d=i` remove placements and
  keep the image data; `d=A` and `d=I` also free it.
- **INV-10** (row 25). The main and alternate screens each have their own
  Kitty keyboard flag stack; the alternate one starts empty.
- **INV-11** (row 26). OSC 9 with a ConEmu subcommand (1 to 12, then `;` or
  the end) raises no desktop notification. `OSC 9;42` still does.
- **INV-12** (row 27). An iTerm2 `width=` or `height=` that is not a bare
  number of cells (`100px`, `50%`, `auto`) leaves the default size.
- **INV-13** (row 28). Under DECOM, CPR reports the row relative to the
  scroll region's top.
- **INV-14** (rows 29, 1296). RIS keeps the SGR debug log on, with its file
  handle, and keeps the scrollback-insert pause the widget set.
- **INV-15** (row 19). An inline image with no named format decodes only
  from a PNG, JPEG or GIF signature; other bytes never reach Qt's format
  detection.

## Test

`test_terminalgrid_review_fixes.cpp`, in the `test_vt` bundle. Each
invariant drives `TerminalGrid` through `VtParser`, or `processAction` where
the parser cannot produce the action (a truncated OSC).
