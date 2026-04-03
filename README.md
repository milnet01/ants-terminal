# Ants Terminal

<p align="center">
  <img src="assets/ants-terminal-128.png" alt="Ants Terminal Icon" width="128" height="128">
</p>

<p align="center">
  <strong>A real terminal emulator built from scratch in C++ with Qt6.</strong><br>
  No terminal libraries — custom VT100/xterm parser, PTY management, and cell-by-cell rendering.
</p>

<p align="center">
  <a href="#features">Features</a> &bull;
  <a href="#building">Building</a> &bull;
  <a href="#keyboard-shortcuts">Shortcuts</a> &bull;
  <a href="#themes">Themes</a> &bull;
  <a href="#architecture">Architecture</a> &bull;
  <a href="#configuration">Configuration</a> &bull;
  <a href="#license">License</a>
</p>

---

## Features

### Terminal Emulation

- **Real shell** — spawns your login shell (bash, zsh, etc.) via `forkpty()` with full interactive support
- **VT100/xterm compatible** — custom state machine parser based on the Paul Williams DEC model
- **UTF-8** — complete 1-4 byte decoding; invalid sequences replaced with U+FFFD
- **Alt screen buffer** — vim, htop, less, nano, and other full-screen programs work correctly
- **Scrollback history** — 10,000-line circular buffer, navigable by mouse wheel or keyboard
- **Bracketed paste mode** — supported for shells and editors that use it
- **Window title** — reads OSC 0/2 title sequences from your shell prompt or running programs
- **Dynamic resize** — window size changes propagate to the PTY via `SIGWINCH`

### Color Support

| Mode | Range | Description |
|------|-------|-------------|
| Standard ANSI | 16 colors | Overridable per theme |
| Extended 256 | 256 colors | 6x6x6 RGB cube + 24 grayscale |
| True Color | 16.7M colors | 24-bit RGB via `38;2;R;G;B` / `48;2;R;G;B` SGR |

Sets `TERM=xterm-256color` and `COLORTERM=truecolor` so programs detect capabilities automatically.

### Text Attributes

Bold, italic, underline, dim, inverse, and strikethrough — all rendered natively with font variants and QPainter drawing.

### Selection and Copy/Paste

- **Click and drag** to select text across lines
- **Double-click** to select a word
- **Ctrl+Shift+C** to copy selection to clipboard
- **Ctrl+Shift+V** to paste text (or images) from clipboard
- **Middle-click** to paste the X11 primary selection
- **Shift+Enter** inserts a literal newline without executing (multi-line command editing)

### Image Paste

Paste images directly from the clipboard with **Ctrl+Shift+V**. Images are saved as timestamped PNGs to `/tmp/ants-terminal/` and the path is shown in the status bar.

### URL Detection

URLs (`http://`, `https://`, `ftp://`, `file://`) are automatically detected, underlined, and colored with the theme accent. Hold **Ctrl** and the cursor changes to a pointing hand — **Ctrl+Click** opens the URL in your default browser.

### Search in Scrollback

Press **Ctrl+Shift+F** to open the search bar. Type a query to highlight all case-insensitive matches across the entire scrollback and visible screen.

- **Enter** — jump to the next match
- **Shift+Enter** — jump to the previous match
- **Escape** — close the search bar
- Match counter shows `N of Total` in the search bar

All matches are highlighted in yellow; the current match is highlighted in orange.

### Bracket Matching

When the cursor sits on a bracket character — `(`, `)`, `[`, `]`, `{`, or `}` — the matching bracket is highlighted with a subtle background. The scanner handles arbitrary nesting depth and works across lines.

### 7 Built-in Themes

Switch themes from the **View** menu. Your choice is saved between sessions.

| Theme | Background | Accent | Style |
|-------|-----------|--------|-------|
| **Dark** (default) | `#1E1E2E` | `#89B4FA` | Catppuccin Mocha |
| **Light** | `#FFFFFF` | `#1A73E8` | Clean light |
| **Nord** | `#2E3440` | `#88C0D0` | Arctic, blue-green |
| **Dracula** | `#282A36` | `#BD93F9` | Purple accent |
| **Monokai** | `#272822` | `#66D9EF` | Warm classic |
| **Solarized Dark** | `#002B36` | `#268BD2` | Ethan Schoonover |
| **Gruvbox** | `#282828` | `#FABD2F` | Retro warm |

Each theme includes a full 16-color ANSI palette override, so `ls --color`, shell prompts, and colorized output all look consistent.

### Window Management

- **Persistent geometry** — window size and position saved/restored between sessions
- **Center window** — **Ctrl+Shift+C** or View menu to center on your current monitor
- **Zoom** — **Ctrl+=** / **Ctrl+-** / **Ctrl+0** (8pt to 32pt range, saved to config)
- **Minimum size** enforced (20 columns x 5 rows)

### Fonts

Ants Terminal tries the following monospace fonts in order, using the first one available on your system:

1. JetBrains Mono
2. Fira Code
3. Source Code Pro
4. DejaVu Sans Mono
5. Liberation Mono
6. Monospace (system fallback)

Default size: **11pt**. Zoom range: **8-32pt**.

### Input Method

Full Qt Input Method support for CJK and other complex input methods. Cursor position is reported to the IME for correct popup placement.

---

## Building

### Dependencies

| Dependency | Version | Notes |
|-----------|---------|-------|
| C++ compiler | C++20 (GCC 12+, Clang 15+) | |
| Qt6 | 6.x | Core, Gui, Widgets modules |
| CMake | 3.20+ | |
| libutil | — | Included with glibc (provides `forkpty`) |

### Install Dependencies

**openSUSE Tumbleweed:**
```bash
sudo zypper install qt6-base-devel qt6-widgets-devel cmake gcc-c++
```

**Ubuntu / Debian:**
```bash
sudo apt install qt6-base-dev cmake g++
```

**Fedora:**
```bash
sudo dnf install qt6-qtbase-devel cmake gcc-c++
```

**Arch Linux:**
```bash
sudo pacman -S qt6-base cmake gcc
```

### Compile

```bash
git clone https://github.com/milnet01/ants-terminal.git
cd ants-terminal
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Run

```bash
./ants-terminal
```

### Install System-wide

```bash
sudo make install   # installs to /usr/local/bin/ants-terminal
```

---

## Keyboard Shortcuts

### General

| Shortcut | Action |
|----------|--------|
| Ctrl+Shift+N | New window |
| Ctrl+Shift+Q | Quit |

### Editing

| Shortcut | Action |
|----------|--------|
| Shift+Enter | Insert literal newline (multi-line editing) |
| Ctrl+Shift+C | Copy selection to clipboard |
| Ctrl+Shift+V | Paste from clipboard (text or image) |
| Middle-click | Paste X11 primary selection |

### Navigation

| Shortcut | Action |
|----------|--------|
| Mouse wheel | Scroll through history |
| Shift+Page Up | Scroll up half a screen |
| Shift+Page Down | Scroll down half a screen |

### Search

| Shortcut | Action |
|----------|--------|
| Ctrl+Shift+F | Open / close search bar |
| Enter (in search) | Next match |
| Shift+Enter (in search) | Previous match |
| Escape | Close search bar |

### View

| Shortcut | Action |
|----------|--------|
| Ctrl+= | Zoom in |
| Ctrl+- | Zoom out |
| Ctrl+0 | Reset zoom (11pt) |
| Ctrl+Shift+C | Center window on screen |

### URL Interaction

| Action | How |
|--------|-----|
| Ctrl+Click on URL | Open in default browser |
| Ctrl held over URL | Cursor changes to pointing hand |

### Mouse

| Action | How |
|--------|-----|
| Click + drag | Select text |
| Double-click | Select word |
| Middle-click | Paste primary selection |

---

## Themes

Themes are selectable from the **View > Themes** menu. Each theme defines:

- Primary and secondary background colors
- Primary and secondary text colors
- Accent and border colors
- Cursor color
- Full ANSI 16-color palette (colors 0-15)

The selected theme persists in the config file and is restored on next launch.

---

## Architecture

```
┌──────────┐     ┌──────────┐     ┌──────────────┐     ┌────────────────┐     ┌────────┐
│ Keyboard │────>│   PTY    │────>│   Shell      │────>│   PTY          │────>│ Screen │
│ / Mouse  │     │ (master) │     │ (bash/zsh)   │     │ (master read)  │     │        │
└──────────┘     └──────────┘     └──────────────┘     └───────┬────────┘     └────────┘
                                                               │                   ▲
                                                               ▼                   │
                                                        ┌──────────┐        ┌──────────────┐
                                                        │ VtParser │───────>│TerminalGrid  │
                                                        │          │        │              │
                                                        │ State    │ VtAction│ Char cells  │
                                                        │ machine  │ structs │ + scrollback│
                                                        └──────────┘        └──────┬───────┘
                                                                                   │
                                                                                   ▼
                                                                            ┌──────────────┐
                                                                            │TerminalWidget│
                                                                            │              │
                                                                            │ QPainter     │
                                                                            │ rendering    │
                                                                            └──────────────┘
```

### Components

| Component | File | Responsibility |
|-----------|------|----------------|
| **Pty** | `ptyhandler.h/cpp` | Spawns shell via `forkpty()`, non-blocking I/O with `QSocketNotifier`, PTY resize via `TIOCSWINSZ` |
| **VtParser** | `vtparser.h/cpp` | DEC VT100/xterm state machine, UTF-8 decoding, emits `VtAction` structs (Print, Execute, CsiDispatch, EscDispatch, OscEnd) |
| **TerminalGrid** | `terminalgrid.h/cpp` | Character cell buffer, scrollback ring, cursor state, ANSI colors, alt screen buffer, scroll regions |
| **TerminalWidget** | `terminalwidget.h/cpp` | QPainter cell-by-cell rendering, keyboard/mouse input, selection, URL detection, search, bracket matching |
| **MainWindow** | `mainwindow.h/cpp` | Window chrome, menus, theme switching, config persistence, image paste handling |
| **Themes** | `themes.h/cpp` | 7 color theme definitions with ANSI palette overrides |
| **Config** | `config.h/cpp` | Persistent JSON config at `~/.config/ants-terminal/config.json` with 0600 permissions |

### Data Flow

1. **User types** a key → `TerminalWidget::keyPressEvent()` encodes it and writes to the PTY master fd
2. **Shell processes** the input and produces output (escape sequences, text, etc.)
3. **PTY master** becomes readable → `QSocketNotifier` fires → raw bytes read into buffer
4. **VtParser** feeds bytes through the state machine, emitting `VtAction` structs
5. **TerminalGrid** processes each `VtAction` — prints characters, moves cursor, changes colors, scrolls, etc.
6. **TerminalWidget** calls `update()` → `paintEvent()` iterates the grid and paints each cell with QPainter

---

## Supported Escape Sequences

### CSI Sequences (`ESC [`)

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
| n | DSR | Device status report |
| r | DECSTBM | Set scrolling region |
| s | DECSC | Save cursor position |
| u | DECRC | Restore cursor position |

### Private Modes (`ESC [ ? ... h/l`)

| Mode | Name | Description |
|------|------|-------------|
| 1 | DECCKM | Application cursor keys |
| 6 | DECOM | Origin mode |
| 7 | DECAWM | Auto-wrap mode |
| 25 | DECTCEM | Cursor visibility |
| 47 | — | Alt screen buffer |
| 1047 | — | Alt screen buffer (xterm) |
| 1049 | — | Alt screen + save cursor |
| 2004 | — | Bracketed paste mode |

### ESC Sequences

| Code | Name | Description |
|------|------|-------------|
| M | RI | Reverse index (scroll down) |
| D | IND | Index (scroll up) |
| E | NEL | Next line |
| 7 | DECSC | Save cursor |
| 8 | DECRC | Restore cursor |
| c | RIS | Full reset |

### OSC Sequences (`ESC ]`)

| Code | Description |
|------|-------------|
| 0 | Set window title |
| 2 | Set window title |

### SGR Attributes (`ESC [ ... m`)

| Code | Attribute |
|------|-----------|
| 0 | Reset all |
| 1 | Bold |
| 2 | Dim |
| 3 | Italic |
| 4 | Underline |
| 7 | Inverse |
| 9 | Strikethrough |
| 30-37 | Foreground color |
| 40-47 | Background color |
| 38;5;N | 256-color foreground |
| 48;5;N | 256-color background |
| 38;2;R;G;B | True color foreground |
| 48;2;R;G;B | True color background |
| 90-97 | Bright foreground |
| 100-107 | Bright background |

---

## Configuration

Config is stored at `~/.config/ants-terminal/config.json` with **0600** file permissions (owner read/write only).

```json
{
    "theme": "Dark",
    "font_size": 11,
    "window_w": 900,
    "window_h": 700,
    "window_x": 100,
    "window_y": 100
}
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `theme` | string | `"Dark"` | Active theme name |
| `font_size` | int | `11` | Font size in points (8-32) |
| `window_w` | int | `900` | Window width in pixels |
| `window_h` | int | `700` | Window height in pixels |
| `window_x` | int | `-1` | Window X position (-1 = system default) |
| `window_y` | int | `-1` | Window Y position (-1 = system default) |

The config file is human-readable JSON and can be edited manually. Changes take effect on next launch.

---

## Desktop Integration

### Desktop Entry

A `.desktop` file is provided for Linux desktop environments:

```ini
[Desktop Entry]
Name=Ants Terminal
Comment=A modern terminal with themes, image support, and more
Exec=/path/to/launch.sh
Icon=ants-terminal
Terminal=false
Type=Application
Categories=Utility;TerminalEmulator;
Keywords=terminal;chat;ants;
StartupWMClass=ants-terminal
```

### Setup

1. Copy `ants-terminal.desktop` to `~/.local/share/applications/`
2. Update the `Exec=` path to point to your `launch.sh`
3. Install icons from `assets/` to `~/.local/share/icons/` if desired

The `launch.sh` wrapper script is required for reliable launching from application menus and taskbars — it sets up the execution environment correctly and logs errors to `/tmp/ants-terminal.log`.

---

## Project Structure

```
ants-terminal/
├── CMakeLists.txt              # Build configuration
├── README.md                   # This file
├── LICENSE                     # MIT License
├── CLAUDE.md                   # Development context
├── STANDARDS.md                # Coding standards
├── RULES.md                    # Development rules
├── launch.sh                   # Desktop launcher wrapper
├── ants-terminal.desktop       # Desktop entry file
├── assets/                     # App icons (16-256px PNGs)
│   ├── ants-terminal-16.png
│   ├── ants-terminal-32.png
│   ├── ants-terminal-48.png
│   ├── ants-terminal-64.png
│   ├── ants-terminal-128.png
│   ├── ants-terminal-256.png
│   └── ants-terminal.png
└── src/
    ├── main.cpp                # Entry point, Fusion style, font setup
    ├── mainwindow.h/cpp        # Window, menus, theme switching, config
    ├── terminalwidget.h/cpp    # Rendering, input, selection, search, URLs
    ├── terminalgrid.h/cpp      # Cell grid, scrollback, VtAction processing
    ├── vtparser.h/cpp          # VT100/xterm state machine, UTF-8 decoder
    ├── ptyhandler.h/cpp        # PTY via forkpty(), QSocketNotifier I/O
    ├── themes.h/cpp            # 7 color themes with ANSI palette overrides
    └── config.h/cpp            # JSON config persistence (0600 perms)
```

---

## Security

- **Config file permissions**: created with `0600` (owner read/write only)
- **Config directory permissions**: created with `0700`
- **No network access**: Ants Terminal makes no network connections — URLs are opened via the system's default browser handler
- **No eval/exec of untrusted input**: escape sequences are parsed through a strict state machine; unrecognized sequences are silently discarded
- **Validated config values**: font size clamped to 8-32pt range, theme names validated against known list

---

## Contributing

Contributions are welcome! This project is built from scratch with no terminal library dependencies, so understanding the VT100 state machine and PTY layer is helpful context.

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Build and test: `mkdir build && cd build && cmake .. && make -j$(nproc)`
5. Submit a pull request

---

## License

[MIT License](LICENSE) -- Copyright (c) 2026 Ants Terminal Contributors
