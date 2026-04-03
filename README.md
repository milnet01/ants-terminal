# Ants Terminal

A real terminal emulator built from scratch in C++ with Qt6. No terminal libraries — custom VT100 parser, PTY management, and character grid rendering.

## Features

- **Real Terminal** — runs your shell (bash/zsh), executes commands, supports interactive programs
- **7 Built-in Themes** — Dark, Light, Nord, Dracula, Monokai, Solarized Dark, Gruvbox
- **Image Paste Support** — Ctrl+Shift+V to paste images from clipboard
- **Shift+Enter** — insert a literal newline without executing (for multi-line commands)
- **Scrollback** — 10,000 line history, scroll with mouse wheel or Shift+PgUp/PgDn
- **256/True Color** — full SGR color support including 24-bit RGB
- **Alt Screen Buffer** — vim, htop, less, etc. work correctly
- **Persistent Config** — window size, position, theme saved between sessions
- **Window Title** — reads OSC title sequences from your shell

## Building

### Requirements

- C++20 compiler (GCC 12+ or Clang 15+)
- Qt6 (Core, Gui, Widgets)
- CMake 3.20+
- libutil (usually included with glibc)

### openSUSE Tumbleweed

```bash
sudo zypper install qt6-base-devel qt6-widgets-devel cmake gcc-c++
```

### Build

```bash
cd /path/to/ants-terminal
mkdir build && cd build
cmake ..
make -j$(nproc)
./ants-terminal
```

### Install

```bash
sudo make install   # installs to /usr/local/bin/ants-terminal
```

## Keyboard Shortcuts

| Shortcut | Action |
|---|---|
| Shift+Enter | New line (without executing) |
| Ctrl+Shift+V | Paste (text or image) |
| Ctrl+Shift+C | Copy (TODO) |
| Ctrl+Shift+N | New window |
| Ctrl+= | Zoom in |
| Ctrl+- | Zoom out |
| Ctrl+0 | Reset zoom |
| Ctrl+Shift+C | Center window |
| Shift+PgUp/PgDn | Scroll history |
| Mouse wheel | Scroll history |
| Ctrl+Shift+Q | Quit |

## Architecture

```
Keyboard → PTY (master) → Shell (bash) → PTY (master) → VtParser → TerminalGrid → TerminalWidget → Screen
```

- **PTY**: forkpty() with non-blocking I/O via QSocketNotifier
- **VtParser**: DEC VT100/xterm state machine with UTF-8 decoding
- **TerminalGrid**: character cell buffer, ANSI colors, scrollback, cursor, alt screen
- **TerminalWidget**: QPainter rendering, keyboard input handling

## License

MIT
