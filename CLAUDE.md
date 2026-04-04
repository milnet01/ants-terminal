# Ants Terminal

A real terminal emulator built from scratch in C++ with Qt6.

## Tech Stack

- **C++20** with **Qt6** (Widgets, Core, Gui)
- **libutil** for PTY (forkpty)
- CMake build system

## Project Structure

```
├── CMakeLists.txt
├── src/
│   ├── main.cpp              # Entry point
│   ├── mainwindow.h/.cpp     # Main window, menus, theme switching
│   ├── terminalwidget.h/.cpp # Qt widget: renders grid, handles keyboard input
│   ├── terminalgrid.h/.cpp   # Character grid, scrollback, processes VtActions
│   ├── vtparser.h/.cpp       # VT100/xterm escape sequence state machine
│   ├── pty.h/.cpp            # PTY management (forkpty, read/write, resize)
│   ├── themes.h/.cpp         # 7 color themes with ANSI palette overrides
│   └── config.h/.cpp         # Persistent JSON config
├── assets/                   # Icons
├── STANDARDS.md
├── RULES.md
└── README.md
```

## Architecture

Data flows: **PTY → VtParser → TerminalGrid → TerminalWidget (QPainter)**
Reverse flow: **TerminalGrid → ResponseCallback → PTY** (for DA, CPR, DSR)

- `Pty`: forkpty() spawns bash, QSocketNotifier for non-blocking reads
- `VtParser`: state machine decodes UTF-8, emits VtAction structs (Print, Execute, CSI, ESC, OSC)
- `TerminalGrid`: processes VtActions, maintains cell grid + scrollback + cursor + modes
  - Mouse/focus/sync modes, OSC 8 hyperlinks, OSC 133 shell integration
  - Combining chars stored in per-line side table (`TermLine::combining`)
  - Response callback for DA1/DA2/CPR/DSR sequences
- `TerminalWidget`: renders grid with QPainter, handles keyboard/mouse → PTY writes
  - SGR mouse reporting, focus reporting, synchronized output
  - Undercurl + colored underlines, font fallback, image paste auto-save
- `MainWindow`: wires everything, manages themes/config/menus/keybindings

## Building

```bash
mkdir build && cd build
cmake .. && make -j$(nproc)
./ants-terminal
```

## Conventions

- Signals/slots for cross-component communication
- Theme colors set on TerminalGrid (default fg/bg), used during rendering
- ANSI palette (16 standard + 216 cube + 24 gray) initialized in TerminalGrid
- Config persists to ~/.config/ants-terminal/config.json with 0600 permissions
- Scrollback configurable up to 1,000,000 lines (default 50,000)

## Key Design Decisions

- Custom VT100 parser (not pyte/libvterm) — no external deps beyond Qt6
- QPainter cell-by-cell + text-run rendering (CPU) — simple, correct, supports ligatures
- Alt screen buffer support (for vim, htop, etc.)
- Delayed wrap (like xterm) for correct line-wrapping behavior
- SGR mouse reporting, focus reporting, synchronized output for TUI app compatibility
- Combining chars in per-line side table (zero memory overhead for lines without combiners)
- Image paste auto-saves to disk and inserts filepath (optimized for Claude Code workflow)
- Keybindings configurable via config.json
