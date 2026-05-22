# Supported escape sequences

Reference for the VT100/xterm escape sequences Ants Terminal's parser
recognises. Moved out of the README (ANTS-1298) to keep the front page
lean. The parser is a custom state machine (`src/vtparser.cpp`) based on
the Paul Williams DEC model — no `libvterm`/`pyte` dependency.

## CSI sequences (`ESC [`)

| Code | Name | Description |
|------|------|-------------|
| A | CUU | Cursor up |
| B | CUD | Cursor down |
| C | CUF | Cursor forward |
| D | CUB | Cursor backward |
| E | CNL | Cursor next line |
| F | CPL | Cursor previous line |
| G | CHA | Cursor horizontal absolute |
| H | CUP | Cursor position |
| J | ED | Erase in display (0/1/2/3) |
| K | EL | Erase in line (0/1/2) |
| L | IL | Insert lines |
| M | DL | Delete lines |
| P | DCH | Delete characters |
| S | SU | Scroll up |
| T | SD | Scroll down |
| X | ECH | Erase characters |
| @ | ICH | Insert blank characters |
| d | VPA | Vertical position absolute |
| f | HVP | Horizontal/vertical position |
| m | SGR | Select graphic rendition |
| c | DA | Device attributes (DA1/DA2 responses) |
| n | DSR | Device status report |
| r | DECSTBM | Set scrolling region |
| s | DECSC | Save cursor position |
| u | DECRC | Restore cursor position |

## Private modes (`ESC [ ? ... h/l`)

| Mode | Description |
|------|-------------|
| 1 | Application cursor keys |
| 6 | Origin mode |
| 7 | Auto-wrap mode |
| 25 | Cursor visibility |
| 47/1047/1049 | Alt screen buffer |
| 1000 | Mouse button reporting |
| 1002 | Mouse button+motion reporting |
| 1003 | Mouse any-motion reporting |
| 1004 | Focus reporting |
| 1006 | SGR mouse encoding |
| 2004 | Bracketed paste mode |
| 2026 | Synchronized output |

## OSC sequences (`ESC ]`)

| Code | Description |
|------|-------------|
| 0/2 | Set window title |
| 8 | Hyperlinks (open/close explicit links) |
| 9 | Desktop notification (body only) |
| 52 | Clipboard access (write only) |
| 133 | Shell integration (A/B/C/D markers) |
| 777 | Desktop notification (title + body) |
| 1337 | iTerm2 inline images |
| 9;4 | Progress indicator (state + percent) |

## DCS / APC sequences

| Protocol | Description |
|----------|-------------|
| DCS (Sixel) | Sixel graphics with palette, RLE, raster attributes |
| APC (Kitty) | Kitty graphics protocol with chunked transmission |

## Advanced underline styles (SGR)

| Style | SGR | Rendering |
|-------|-----|-----------|
| Single | `4` or `4:1` | Straight line |
| Double | `4:2` or `21` | Two parallel lines |
| Curly (undercurl) | `4:3` | Wavy/squiggly line |
| Dotted | `4:4` | Dotted line |
| Dashed | `4:5` | Dashed line |

Underline colour is independently settable via `CSI 58;2;R;G;B m` (used
by Neovim for LSP diagnostic highlights).
