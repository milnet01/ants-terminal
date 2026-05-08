# Feature: DECSTR (`CSI ! p`) — Soft Terminal Reset

## Problem

Conformant TUIs (vim, emacs, neovim, mc, htop) emit DECSTR (`CSI ! p`,
"Soft Terminal Reset") on startup as a defensive recovery sequence —
the user might launch them from a shell whose previous occupant left
the terminal in an inconvenient state (origin mode on, scroll region
narrowed, autowrap off, etc.). DECSTR is the standard "clear my
expectations without nuking the buffer" handshake.

Pre-fix `Ants Terminal` did NOT implement DECSTR. The CSI dispatch
table in `terminalgrid.cpp::handleCsi` had no case for final byte
`'p'` with intermediate `"!"`; the sequence was silently dropped.

This shows up under the same shape as ANTS-1187: a TUI's defensive
recovery is a no-op, the user's prior tab state survives, and output
piles into the wrong region. Concrete trigger: Flask emits no
DECSTBM (per yesterday's `script -q` byte capture), but it doesn't
emit DECSTR either, so the bug only really matters for the broader
contract — *any* shell occupant that uses DECSTR to clean up.

## External anchor

- xterm Control Sequences (`https://invisible-island.net/xterm/ctlseqs/ctlseqs.html`):
  > **DECSTR.** *CSI ! p — Soft terminal reset.*
- DEC VT220 Programmer's Reference Manual:
  > "When the terminal receives the soft terminal reset (DECSTR)
  > control function, the operating environment of the terminal is
  > set to the default state" — list of affected modes follows.

## Reset surface (subset of RIS)

Per xterm + VT220 reference, DECSTR resets:

1. **DECTCEM** (text cursor enable) → cursor visible.
2. **IRM** (insert/replace) → replace.
3. **DECOM** (origin mode) → off.
4. **DECAWM** (auto-wrap) → on.
5. **Scroll margins** (DECSTBM) → top=0, bottom=rows-1.
6. **SGR** → all attributes default.
7. **Saved cursor stack** (DECSC) → cursor pos = (0,0), saved attrs default.
8. **Selection of national replacement charsets** → off.

DECSTR does NOT:

- Wipe the screen or scrollback (RIS does this; DECSTR is "soft").
- Clear hyperlink state, OSC 8 spans, OSC 133 prompts.
- Reset integration callbacks (response, bell, notify, lineCompletion,
  progress, commandFinished, userVar, osc133Forgery, m_osc133Key).
- Reset Kitty keyboard stack (DECSTR predates the protocol).
- Touch alt-screen state — if the active buffer is alt, the soft
  reset applies to alt's modes; the saved primary state in the
  m_alt* slots is preserved.

## Contract

### Invariant 1 — `CSI ! p` resets origin mode

Set DECOM on (`CSI ?6 h`). Send `CSI ! p`. `m_originMode == false`.

### Invariant 2 — `CSI ! p` resets auto-wrap to on

Disable autowrap (`CSI ?7 l`). Send `CSI ! p`. `m_autoWrap == true`.

### Invariant 3 — `CSI ! p` resets scroll region to full screen

Set DECSTBM 5;15 (`CSI 5;15 r`). Send `CSI ! p`.
`scrollTop() == 0 && scrollBottom() == m_rows - 1`.

### Invariant 4 — `CSI ! p` resets SGR

Apply bold red fg (`CSI 1;31 m`). Send `CSI ! p`. Subsequent printed
cell has default attrs (no bold, default fg).

### Invariant 5 — `CSI ! p` makes cursor visible

Hide cursor (`CSI ?25 l`). Send `CSI ! p`. `cursorVisible() == true`.

### Invariant 6 — `CSI ! p` does NOT wipe the screen

Print "hello" at (0,0). Send `CSI ! p`. Cell at (0,0) still reads
'h' (DECSTR is soft — it preserves buffer content, unlike RIS).

### Invariant 7 — `CSI ! p` does NOT clear integration callbacks

Install a response callback. Send `CSI ! p`. Trigger a CPR (`CSI 6 n`).
Callback fires (if it had been cleared, response would silently drop).

### Invariant 8 — `CSI ! p` does NOT clear scrollback

Push N lines into scrollback by exceeding screen height. Send
`CSI ! p`. `scrollbackSize() == N` (unchanged).

## How this test anchors to reality

Direct assertions via `TerminalGrid::scrollTop()` / `scrollBottom()` /
`originMode()` / `autoWrap()` / `cursorVisible()` / `cellAt()` /
`scrollbackSize()` and via a response-callback witness for INV-7.
Drives the parser with the literal byte sequence `\x1b[!p` so the
test exercises the dispatch path end-to-end (intermediate-byte
collection in vtparser → handleCsi finalChar 'p' with intermediate
"!").

## Regression history

- **Pre-0.7.79:** DECSTR not implemented; silently dropped. Users of
  vim/htop/etc. could see scroll-region / origin-mode leak from
  prior shell state.
- **0.7.79 (ANTS-1196 from indie-review #3):** added.
