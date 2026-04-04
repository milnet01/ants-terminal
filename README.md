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
- **UTF-8** — complete 1-4 byte decoding with overlong rejection; invalid sequences replaced with U+FFFD
- **Alt screen buffer** — vim, htop, less, nano, and other full-screen programs work correctly
- **Scrollback history** — configurable up to 1,000,000 lines, navigable by mouse wheel or keyboard
- **Bracketed paste mode** — supported for shells and editors that use it
- **Device Attributes** — responds to DA1 (`CSI c`) and DA2 (`CSI > c`) queries
- **Cursor Position Report** — responds to `CSI 6n` with current cursor position
- **Window title** — reads OSC 0/2 title sequences from your shell prompt or running programs
- **Dynamic resize** — window size changes propagate to the PTY via `SIGWINCH` with scrollback reflow

### Color Support

| Mode | Range | Description |
|------|-------|-------------|
| Standard ANSI | 16 colors | Overridable per theme |
| Extended 256 | 256 colors | 6x6x6 RGB cube + 24 grayscale |
| True Color | 16.7M colors | 24-bit RGB via `38;2;R;G;B` / `48;2;R;G;B` SGR |

Sets `TERM=xterm-256color` and `COLORTERM=truecolor` so programs detect capabilities automatically.

### Text Attributes

Bold, italic, underline, dim, inverse, and strikethrough — all rendered natively with font variants and QPainter drawing.

### Advanced Underline Styles

| Style | SGR | Rendering |
|-------|-----|-----------|
| Single | `4` or `4:1` | Straight line |
| Double | `4:2` or `21` | Two parallel lines |
| Curly (undercurl) | `4:3` | Wavy/squiggly line |
| Dotted | `4:4` | Dotted line |
| Dashed | `4:5` | Dashed line |

Underline color is independently settable via `CSI 58;2;R;G;B m`. Used by Neovim for LSP diagnostic highlights.

### Unicode Support

- **UTF-8** — complete 1-4 byte decoding with overlong encoding rejection
- **CJK double-width** — `wcwidth()` detection, double-cell rendering
- **Combining characters** — diacritical marks attached to base characters (e.g., e + combining acute = e)
- **Emoji** — via font fallback (Noto Color Emoji, Symbola)

### Selection and Copy/Paste

- **Click and drag** to select text across lines
- **Double-click** to select a word
- **Ctrl+Shift+C** to copy selection to clipboard
- **Ctrl+Shift+V** to paste text (or images) from clipboard
- **Middle-click** to paste the X11 primary selection
- **Shift+Enter** inserts a literal newline without executing (multi-line command editing)

### Image Paste (Claude Code Integration)

Paste images directly from the clipboard with **Ctrl+Shift+V**. Images are automatically saved as timestamped PNGs to `~/Pictures/ClaudePaste/` (configurable) and the **full filepath is inserted into the terminal** as text. This enables seamless screenshot sharing with Claude Code — just paste and press Enter.

The save directory is configurable via `image_paste_dir` in `config.json`.

### URL Detection & OSC 8 Hyperlinks

URLs (`http://`, `https://`, `ftp://`, `file://`) are automatically detected, underlined, and colored with the theme accent. Hold **Ctrl** and the cursor changes to a pointing hand — **Ctrl+Click** opens the URL in your default browser.

Applications can also emit **explicit hyperlinks** via the OSC 8 protocol (`ESC ] 8 ; params ; URI ST`). These take priority over regex-detected URLs and support grouped/multiline links via the `id` parameter.

### Search in Scrollback

Press **Ctrl+Shift+F** to open the search bar. Type a query to highlight all case-insensitive matches across the entire scrollback and visible screen.

- **Enter** — jump to the next match
- **Shift+Enter** — jump to the previous match
- **Escape** — close the search bar
- Match counter shows `N of Total` in the search bar

All matches are highlighted in yellow; the current match is highlighted in orange.

### Bracket Matching

When the cursor sits on a bracket character — `(`, `)`, `[`, `]`, `{`, or `}` — the matching bracket is highlighted with a subtle background. The scanner handles arbitrary nesting depth and works across lines.

### Mouse Reporting

Full SGR mouse reporting for TUI applications:

- **Button events** (`?1000h`) — press and release
- **Button+motion** (`?1002h`) — drag tracking
- **Any motion** (`?1003h`) — full mouse position tracking
- **SGR encoding** (`?1006h`) — unlimited coordinates, explicit release

Works with htop, lazygit, ranger, fzf, tmux, and other mouse-aware applications. Hold **Shift** to override mouse reporting and use terminal selection instead.

### Focus Reporting

When enabled (`CSI ?1004h`), the terminal sends `CSI I` on focus gain and `CSI O` on focus loss. Used by Vim, Neovim, and tmux to detect when the terminal window is active.

### Synchronized Output

Applications can bracket rapid output updates with `CSI ?2026h` / `CSI ?2026l` to prevent screen tearing. The terminal defers repainting until the end marker arrives.

### Shell Integration (OSC 133)

When your shell emits OSC 133 markers (A/B/C/D), Ants Terminal tracks prompt regions. This enables:

- Command output boundaries detection
- Prompt navigation (future: Ctrl+Shift+Up/Down to jump between prompts)
- Exit status tracking

### Clipboard Access (OSC 52)

Remote applications can set the system clipboard via OSC 52 sequences. Clipboard read (query) is disabled for security.

### Background Opacity & Blur

- **Opacity** — adjustable from 70% to 100% via View menu (saved to config)
- **Translucent background** — works with KDE Plasma / KWin compositor
- **Background blur** — toggleable in Settings menu (requires compositor support)

### Configurable Keybindings

All keyboard shortcuts can be customized via the `keybindings` section in `config.json`. Set any action to your preferred key combination:

```json
{
    "keybindings": {
        "new_tab": "Ctrl+Shift+T",
        "close_tab": "Ctrl+Shift+W",
        "split_horizontal": "Ctrl+Shift+E"
    }
}
```

### Font Fallback

The terminal automatically detects and uses fallback fonts for characters not available in the primary font. Falls back to Noto Color Emoji, Noto Sans CJK, or other available Unicode fonts for emoji and CJK characters.

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
| Ctrl+Shift+V | Paste from clipboard (text or image with auto-save) |
| Ctrl+Shift+U | Clear entire input line |
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
| Ctrl+Shift+M | Center window on screen |

### URL Interaction

| Action | How |
|--------|-----|
| Ctrl+Click on URL | Open in default browser |
| Ctrl held over URL | Cursor changes to pointing hand |

### Tabs & Splits

| Shortcut | Action |
|----------|--------|
| Ctrl+Shift+T | New tab |
| Ctrl+Shift+W | Close tab |
| Ctrl+Shift+E | Split horizontal |
| Ctrl+Shift+O | Split vertical |
| Ctrl+Shift+X | Close focused pane |

### Mouse

| Action | How |
|--------|-----|
| Click + drag | Select text (Shift+click overrides mouse reporting) |
| Double-click | Select word |
| Middle-click | Paste primary selection |
| Mouse wheel | Scroll history (or report to app when mouse mode active) |

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
| c | DA | Device attributes (DA1/DA2 responses) |
| n | DSR | Device status report (CPR at `6n`, status at `5n`) |
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
| 1000 | — | Mouse button reporting |
| 1002 | — | Mouse button+motion reporting |
| 1003 | — | Mouse any-motion reporting |
| 1004 | — | Focus reporting (send CSI I / CSI O) |
| 1006 | — | SGR mouse encoding |
| 2004 | — | Bracketed paste mode |
| 2026 | — | Synchronized output (defer repaints) |

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
| 8 | Hyperlinks (open/close explicit links) |
| 52 | Clipboard access (write only; read disabled for security) |
| 133 | Shell integration (A=prompt, B=command, C=output, D=done) |
| 1337 | iTerm2 inline images |

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
| 21 | Double underline |
| 38;2;R;G;B | True color foreground |
| 48;2;R;G;B | True color background |
| 58;2;R;G;B | Underline color (RGB) |
| 58;5;N | Underline color (256-palette) |
| 59 | Reset underline color |
| 4:0 | No underline |
| 4:1 | Single underline |
| 4:2 | Double underline |
| 4:3 | Curly underline (undercurl) |
| 4:4 | Dotted underline |
| 4:5 | Dashed underline |
| 90-97 | Bright foreground |
| 100-107 | Bright background |

---

## Configuration

Config is stored at `~/.config/ants-terminal/config.json` with **0600** file permissions (owner read/write only).

```json
{
    "theme": "Dark",
    "font_size": 11,
    "opacity": 1.0,
    "scrollback_lines": 50000,
    "auto_copy_on_select": true,
    "session_logging": false,
    "background_blur": false,
    "image_paste_dir": "",
    "editor_command": "",
    "keybindings": {
        "new_tab": "Ctrl+Shift+T",
        "close_tab": "Ctrl+Shift+W"
    },
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
| `opacity` | double | `1.0` | Window opacity (0.1-1.0) |
| `scrollback_lines` | int | `50000` | Max scrollback lines (1000-1000000) |
| `auto_copy_on_select` | bool | `true` | Copy to clipboard on text selection |
| `session_logging` | bool | `false` | Log session to file |
| `background_blur` | bool | `false` | Enable KWin background blur |
| `image_paste_dir` | string | `""` | Image paste save directory (default: ~/Pictures/ClaudePaste) |
| `editor_command` | string | `""` | Editor for file path clicking (auto-detects code/kate) |
| `keybindings` | object | `{}` | Custom keybindings (action -> key sequence) |
| `window_w` | int | `900` | Window width in pixels |
| `window_h` | int | `700` | Window height in pixels |
| `window_x` | int | `-1` | Window X position (-1 = system default) |
| `window_y` | int | `-1` | Window Y position (-1 = system default) |

The config file is human-readable JSON and can be edited manually. Changes take effect on next launch.

### Configurable Keybindings

Add a `keybindings` object to config.json. Available actions:

| Action | Default | Description |
|--------|---------|-------------|
| `new_tab` | `Ctrl+Shift+T` | Open new tab |
| `close_tab` | `Ctrl+Shift+W` | Close current tab |
| `new_window` | `Ctrl+Shift+N` | Open new window |
| `exit` | `Ctrl+Shift+Q` | Quit application |
| `clear_line` | `Ctrl+Shift+U` | Clear input line |
| `split_horizontal` | `Ctrl+Shift+E` | Split pane horizontally |
| `split_vertical` | `Ctrl+Shift+O` | Split pane vertically |
| `close_pane` | `Ctrl+Shift+X` | Close focused pane |

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
- **UTF-8 security**: overlong encodings, surrogates, and out-of-range codepoints rejected
- **Bracketed paste**: prevents clipboard injection attacks
- **OSC 52 clipboard**: write-only — clipboard read (query) disabled to prevent data exfiltration
- **Buffer limits**: CSI params capped at 32, inline images capped at 100, combining chars per cell capped at 8
- **OSC title sanitization**: control characters stripped from window titles to prevent UI spoofing

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
