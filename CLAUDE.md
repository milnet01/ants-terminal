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

- `Pty`: forkpty() spawns bash, QSocketNotifier for non-blocking reads
- `VtParser`: state machine decodes UTF-8, emits VtAction structs (Print, Execute, CSI, ESC, OSC)
- `TerminalGrid`: processes VtActions, maintains character cell grid + scrollback + cursor state
- `TerminalWidget`: renders grid with QPainter, handles keyboard → PTY writes
- `MainWindow`: wires everything, manages themes/config/menus

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
- Scrollback capped at 10,000 lines

## Key Design Decisions

- Custom VT100 parser (not pyte/libvterm) — no external deps beyond Qt6
- QPainter cell-by-cell rendering (CPU) — simple, correct, fast enough
- Alt screen buffer support (for vim, htop, etc.)
- Delayed wrap (like xterm) for correct line-wrapping behavior
