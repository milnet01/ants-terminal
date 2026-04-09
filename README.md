# Ants Terminal

<p align="center">
  <img src="assets/ants-terminal-128.png" alt="Ants Terminal Icon" width="128" height="128">
</p>

<p align="center">
  <strong>A real terminal emulator built from scratch in C++ with Qt6.</strong><br>
  No terminal libraries -- custom VT100/xterm parser, PTY management, and GPU-accelerated rendering.
</p>

<p align="center">
  <a href="#features">Features</a> &bull;
  <a href="#claude-code-integration">Claude Code</a> &bull;
  <a href="#building">Building</a> &bull;
  <a href="#keyboard-shortcuts">Shortcuts</a> &bull;
  <a href="#themes">Themes</a> &bull;
  <a href="#architecture">Architecture</a> &bull;
  <a href="#configuration">Configuration</a> &bull;
  <a href="#plugins">Plugins</a> &bull;
  <a href="#license">License</a>
</p>

---

## Features

### Terminal Emulation

- **Real shell** -- spawns your login shell (bash, zsh, etc.) via `forkpty()` with full interactive support
- **VT100/xterm compatible** -- custom state machine parser based on the Paul Williams DEC model
- **UTF-8** -- complete 1-4 byte decoding with overlong rejection; invalid sequences replaced with U+FFFD
- **Alt screen buffer** -- vim, htop, less, nano, and other full-screen programs work correctly
- **Scrollback history** -- configurable up to 1,000,000 lines, navigable by mouse wheel or keyboard
- **Bracketed paste mode** -- supported for shells and editors that use it
- **Device Attributes** -- responds to DA1 (`CSI c`) and DA2 (`CSI > c`) queries
- **Cursor Position Report** -- responds to `CSI 6n` with current cursor position
- **Window title** -- reads OSC 0/2 title sequences from your shell prompt or running programs
- **Dynamic resize** -- window size changes propagate to the PTY via `SIGWINCH` with scrollback reflow

### GPU-Accelerated Rendering

Ants Terminal uses `QOpenGLWidget` for hardware-accelerated rendering out of the box. Two render paths are available:

- **QPainter (default)** -- GPU-accelerated 2D painting via Qt's OpenGL paint engine. Uses `QTextLayout` for proper font ligature shaping.
- **Custom GL Renderer** -- Optional glyph atlas-based renderer with GLSL 3.3 core shaders. Enable via `gpu_rendering: true` in config or Settings menu. Uses a 2048x2048 texture atlas with on-demand glyph rasterization, driver-safe texture swizzle for Intel/AMD compatibility, and per-vertex quad rendering. Note: ligatures are only available in the QPainter path.

### Ligature Support

Text is rendered using `QTextLayout` with HarfBuzz shaping, enabling proper ligatures in fonts like:
- **JetBrains Mono** -- `!=`, `<=`, `>=`, `->`, `=>`, `|>`
- **Fira Code** -- `www`, `::`, `===`, `!==`, `</>`, `<!--`

### Inline Graphics

| Protocol | Support |
|----------|---------|
| **Sixel** | Full DCS parser, two-pass rendering, 4096x4096 max, palette colors |
| **Kitty Graphics** | Chunked base64, PNG/RGBA/RGB, image cache (200 entries), transmit/display/delete |
| **iTerm2** | OSC 1337 inline images, cell-based sizing |

### Color Support

| Mode | Range | Description |
|------|-------|-------------|
| Standard ANSI | 16 colors | Overridable per theme |
| Extended 256 | 256 colors | 6x6x6 RGB cube + 24 grayscale |
| True Color | 16.7M colors | 24-bit RGB via `38;2;R;G;B` / `48;2;R;G;B` SGR |

Sets `TERM=xterm-256color` and `COLORTERM=truecolor` so programs detect capabilities automatically.

### Text Attributes

Bold, italic, underline, dim, inverse, and strikethrough -- all rendered natively with font variants and QPainter drawing.

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

- **UTF-8** -- complete 1-4 byte decoding with overlong encoding rejection
- **CJK double-width** -- `wcwidth()` detection, double-cell rendering
- **Combining characters** -- diacritical marks attached to base characters (e.g., e + combining acute = e)
- **Emoji** -- via font fallback (Noto Color Emoji, Symbola)

### Selection and Copy/Paste

- **Click and drag** to select text across lines
- **Double-click** to select a word
- **Ctrl+Shift+C** to copy selection to clipboard
- **Ctrl+Shift+V** to paste text (or images) from clipboard
- **Middle-click** to paste the X11 primary selection
- **Shift+Enter** inserts a literal newline without executing (multi-line command editing)

### Image Paste (Claude Code Integration)

Paste images directly from the clipboard with **Ctrl+Shift+V**. Images are automatically saved as timestamped PNGs to `~/Pictures/ClaudePaste/` (configurable) and the **full filepath is inserted into the terminal** as text. This enables seamless screenshot sharing with Claude Code -- just paste and press Enter.

### URL Detection & OSC 8 Hyperlinks

URLs (`http://`, `https://`, `ftp://`, `file://`) are automatically detected, underlined, and colored with the theme accent. Hold **Ctrl** and the cursor changes to a pointing hand -- **Ctrl+Click** opens the URL in your default browser.

Applications can also emit **explicit hyperlinks** via the OSC 8 protocol (`ESC ] 8 ; params ; URI ST`). These take priority over regex-detected URLs.

### Search in Scrollback

Press **Ctrl+Shift+F** to open the search bar. All case-insensitive matches are highlighted across the entire scrollback and visible screen.

### Command Palette

Press **Ctrl+Shift+P** to open the command palette -- a searchable list of all available actions with their keyboard shortcuts. Type to filter, arrow keys to navigate, Enter to execute.

### Bracket Matching

When the cursor sits on a bracket character -- `(`, `)`, `[`, `]`, `{`, or `}` -- the matching bracket is highlighted with a subtle background.

### Line Bookmarks

Toggle bookmarks on any line with **Ctrl+Shift+B**. Navigate between bookmarks with **Ctrl+Shift+N** (next) and **Ctrl+Shift+K** (previous). Bookmarks are shown as colored dots in the left gutter.

### Prompt Navigation (OSC 133)

When your shell emits OSC 133 markers, jump between command prompts with **Ctrl+Shift+Up/Down**.

### Mouse Reporting

Full SGR mouse reporting for TUI applications:

- **Button events** (`?1000h`) -- press and release
- **Button+motion** (`?1002h`) -- drag tracking
- **Any motion** (`?1003h`) -- full mouse position tracking
- **SGR encoding** (`?1006h`) -- unlimited coordinates, explicit release

Hold **Shift** to override mouse reporting and use terminal selection instead.

### Focus Reporting

When enabled (`CSI ?1004h`), the terminal sends `CSI I` on focus gain and `CSI O` on focus loss.

### Synchronized Output

Applications can bracket rapid output updates with `CSI ?2026h` / `CSI ?2026l` to prevent screen tearing.

### Clipboard Access (OSC 52)

Remote applications can set the system clipboard via OSC 52 sequences. Clipboard read (query) is disabled for security.

### Session Recording

Record terminal sessions in **asciicast v2** format via **Ctrl+Shift+R** or the Settings menu. Recordings are saved to `~/.local/share/ants-terminal/recordings/` and can be played back with [asciinema](https://asciinema.org).

### Session Persistence

Enable in Settings to save terminal scrollback on exit and restore it on next launch. Session data is compressed and stored in `~/.local/share/ants-terminal/sessions/`.

### Per-Pixel Background Transparency

Two levels of transparency control:

- **Window Opacity** (View > Opacity) -- affects the entire window including title bar
- **Background Alpha** (View > Background Alpha) -- only the terminal background becomes transparent while text remains fully opaque. Works with KDE/KWin compositor blur.

### AI Assistant

Press **Ctrl+Shift+A** to open the AI assistant. Ask questions about terminal output, get command suggestions, or debug errors. Supports any OpenAI-compatible API endpoint (Ollama, LM Studio, OpenAI, Anthropic via proxy).

Configure in `config.json`:
```json
{
    "ai_endpoint": "http://localhost:11434/v1/chat/completions",
    "ai_model": "llama3",
    "ai_api_key": ""
}
```

Features:
- Streaming responses (SSE)
- Terminal context injection (last N lines)
- "Insert Command" button to paste AI suggestions into the terminal
- Works with local LLMs (no data leaves your machine with Ollama)

### SSH Manager

Press **Ctrl+Shift+S** to open the SSH manager. Manage connection bookmarks and connect to remote hosts.

Features:
- Quick connect bar (`user@host:port`)
- Bookmark management (save, edit, delete)
- Connect in new tab or current tab
- Identity file and extra SSH args support
- Passwords are never stored -- authentication is interactive

### Scriptable Plugin System (Lua)

Extend Ants Terminal with Lua plugins. Plugins react to terminal events and can send commands, show notifications, and modify behavior.

**Plugin API:**
```lua
-- ~/.config/ants-terminal/plugins/my-plugin/init.lua
ants.on("output", function(data)
    ants.log("Got output: " .. data)
end)

ants.on("keypress", function(key)
    -- Return false to cancel the keypress
    return true
end)

ants.send("echo hello")
ants.notify("Title", "Message")
ants.get_output(50)  -- Last 50 lines
ants.get_cwd()       -- Current directory
ants.set_status("Custom status text")
```

**Events:** `output`, `line`, `prompt`, `keypress`, `title_changed`, `tab_created`, `tab_closed`

**Security:** Plugins run in a sandbox with `os`, `io`, `debug`, `require`, `setmetatable`, `collectgarbage` removed. 10M instruction timeout prevents infinite loops (immune to `pcall` bypass). Memory capped at 10MB to prevent `string.rep` OOM attacks.

### Background Opacity & Blur

- **Opacity** -- adjustable from 70% to 100% via View menu (saved to config)
- **Background Alpha** -- per-pixel transparency from 50% to 100%
- **Translucent background** -- works with KDE Plasma / KWin compositor
- **Background blur** -- toggleable in Settings menu (requires compositor support)

### Claude Code Integration

Deep integration with [Claude Code](https://claude.ai/claude-code) for AI-assisted development workflows.

**Live Status Monitoring** -- The status bar shows Claude's current state (idle, thinking, tool use) with context window usage. Process detection via `/proc` polling automatically tracks running Claude sessions.

**Project & Session Browser** (**Ctrl+Shift+J**) -- Browse all your Claude Code projects discovered from `~/.claude/projects/`. For each project, see every session with its first message summary, timestamp, file size, and active status. Resume any session, continue the latest, or fork a session -- all directly from the terminal.

**Permission Allowlist Editor** (**Ctrl+Shift+L**) -- Visual editor for `.claude/settings.local.json` permission rules. Add, remove, and organize allow/deny/ask rules with preset suggestions for common tools (git, npm, cargo, etc.). Changes are written atomically with proper file permissions.

**Session Transcript Viewer** -- Read-only viewer for Claude Code JSONL transcripts. Displays user messages, assistant responses, tool calls, and token usage in a formatted HTML view.

**Slash Command Shortcuts** -- Quick menu entries for sending `/compact`, `/clear`, `/cost`, `/help`, and `/status` to a running Claude session.

**Project Directory Management** -- Configure directories where new Claude Code projects can be created. Start new projects in any managed directory directly from the Projects dialog.

### Configurable Keybindings

All keyboard shortcuts can be customized via the `keybindings` section in `config.json`.

### Font Fallback

The terminal automatically detects and uses fallback fonts for characters not available in the primary font.

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

### Window Management

- **Persistent geometry** -- window size and position saved/restored between sessions
- **Center window** -- **Ctrl+Shift+M** or View menu
- **Zoom** -- **Ctrl+=** / **Ctrl+-** / **Ctrl+0** (8pt to 32pt range)
- **Tabs** -- Ctrl+Shift+T/W for new/close
- **Split panes** -- horizontal (Ctrl+Shift+E) and vertical (Ctrl+Shift+O)

---

## Building

### Dependencies

| Dependency | Version | Notes |
|-----------|---------|-------|
| C++ compiler | C++20 (GCC 12+, Clang 15+) | |
| Qt6 | 6.x | Core, Gui, Widgets, Network, OpenGL, OpenGLWidgets |
| CMake | 3.20+ | |
| libutil | -- | Included with glibc (provides `forkpty`) |
| Lua 5.4 | 5.4.x | Optional -- enables plugin system |

### Install Dependencies

**openSUSE Tumbleweed:**
```bash
sudo zypper install qt6-base-devel cmake gcc-c++ lua54-devel
```

**Ubuntu / Debian:**
```bash
sudo apt install qt6-base-dev libqt6opengl6-dev cmake g++ liblua5.4-dev
```

**Fedora:**
```bash
sudo dnf install qt6-qtbase-devel cmake gcc-c++ lua-devel
```

**Arch Linux:**
```bash
sudo pacman -S qt6-base cmake gcc lua
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
| Ctrl+Shift+P | Command palette |

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
| Shift+Page Up/Down | Scroll by screen |
| Shift+Home/End | Scroll to top/bottom |
| Ctrl+Shift+Up/Down | Jump to prev/next prompt (OSC 133) |
| Ctrl+Shift+B | Toggle line bookmark |
| Ctrl+Shift+N/K | Next/previous bookmark |

### Search

| Shortcut | Action |
|----------|--------|
| Ctrl+Shift+F | Open search bar |
| Enter | Next match |
| Shift+Enter | Previous match |
| Escape | Close search bar |

### View

| Shortcut | Action |
|----------|--------|
| Ctrl+= | Zoom in |
| Ctrl+- | Zoom out |
| Ctrl+0 | Reset zoom (11pt) |
| Ctrl+Shift+M | Center window on screen |

### Tabs & Splits

| Shortcut | Action |
|----------|--------|
| Ctrl+Shift+T | New tab |
| Ctrl+Shift+W | Close tab |
| Ctrl+Shift+E | Split horizontal |
| Ctrl+Shift+O | Split vertical |
| Ctrl+Shift+X | Close focused pane |

### Tools

| Shortcut | Action |
|----------|--------|
| Ctrl+Shift+A | AI Assistant |
| Ctrl+Shift+S | SSH Manager |
| Ctrl+Shift+R | Toggle session recording |
| Ctrl+Shift+J | Claude Code Projects & Sessions |
| Ctrl+Shift+L | Claude Code Allowlist Editor |

---

## Themes

Themes are selectable from the **View > Themes** menu. Each theme defines:

- Primary and secondary background colors
- Primary and secondary text colors
- Accent and border colors
- Cursor color
- Full ANSI 16-color palette (colors 0-15)

---

## Architecture

```
┌──────────┐     ┌──────────┐     ┌──────────────┐     ┌────────────────┐
│ Keyboard │────>│   PTY    │────>│   Shell      │────>│   PTY          │
│ / Mouse  │     │ (master) │     │ (bash/zsh)   │     │ (master read)  │
└──────────┘     └──────────┘     └──────────────┘     └───────┬────────┘
                                                               │
                                                               v
                                                        ┌──────────┐
                                                        │ VtParser │
                                                        │ State    │
                                                        │ machine  │
                                                        └────┬─────┘
                                                             │ VtAction
                                                             v
                                                      ┌──────────────┐
                                                      │TerminalGrid  │
                                                      │ Cells +      │
                                                      │ scrollback   │
                                                      └──────┬───────┘
                                                             │
                                          ┌──────────────────┼──────────────────┐
                                          v                  v                  v
                                   ┌──────────┐     ┌──────────────┐    ┌──────────┐
                                   │ QPainter │     │ GlRenderer   │    │ Session  │
                                   │ (CPU)    │     │ (GPU/OpenGL) │    │ Manager  │
                                   └──────────┘     └──────────────┘    └──────────┘
```

### Components

| Component | File | Responsibility |
|-----------|------|----------------|
| **Pty** | `ptyhandler.h/cpp` | Spawns shell via `forkpty()`, non-blocking I/O, PTY resize |
| **VtParser** | `vtparser.h/cpp` | DEC VT100/xterm state machine, UTF-8 decoding, emits VtActions |
| **TerminalGrid** | `terminalgrid.h/cpp` | Cell buffer, scrollback, cursor, ANSI colors, alt screen, Sixel/Kitty |
| **TerminalWidget** | `terminalwidget.h/cpp` | QOpenGLWidget rendering, input, selection, search, URLs |
| **GlRenderer** | `glrenderer.h/cpp` | OpenGL glyph atlas, GLSL 3.3 shaders, per-vertex rendering |
| **MainWindow** | `mainwindow.h/cpp` | Window chrome, menus, themes, config, dialogs |
| **AiDialog** | `aidialog.h/cpp` | AI chat dialog, OpenAI API, streaming SSE |
| **SshDialog** | `sshdialog.h/cpp` | SSH bookmark manager, connection via PTY |
| **SessionManager** | `sessionmanager.h/cpp` | Scrollback serialization, save/restore |
| **LuaEngine** | `luaengine.h/cpp` | Embedded Lua 5.4, sandboxed API, event handlers |
| **PluginManager** | `pluginmanager.h/cpp` | Plugin discovery, loading, lifecycle |
| **CommandPalette** | `commandpalette.h/cpp` | Searchable action overlay (Ctrl+Shift+P) |
| **TitleBar** | `titlebar.h/cpp` | Custom frameless title bar with drag support |
| **XcbPositionTracker** | `xcbpositiontracker.h/cpp` | Window position tracking via KWin scripting |
| **ClaudeIntegration** | `claudeintegration.h/cpp` | Claude Code process detection, status, hooks |
| **ClaudeProjects** | `claudeprojects.h/cpp` | Project/session browser and resume dialog |
| **ClaudeAllowlist** | `claudeallowlist.h/cpp` | Permission rule editor for Claude settings |
| **ClaudeTranscript** | `claudetranscript.h/cpp` | Session transcript viewer |
| **Themes** | `themes.h/cpp` | 7 color themes with ANSI palette overrides |
| **Config** | `config.h/cpp` | JSON config persistence (0600 perms) |

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
| n | DSR | Device status report |
| r | DECSTBM | Set scrolling region |
| s | DECSC | Save cursor position |
| u | DECRC | Restore cursor position |

### Private Modes (`ESC [ ? ... h/l`)

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

### OSC Sequences (`ESC ]`)

| Code | Description |
|------|-------------|
| 0/2 | Set window title |
| 8 | Hyperlinks (open/close explicit links) |
| 52 | Clipboard access (write only) |
| 133 | Shell integration (A/B/C/D markers) |
| 1337 | iTerm2 inline images |

### DCS / APC Sequences

| Protocol | Description |
|----------|-------------|
| DCS (Sixel) | Sixel graphics with palette, RLE, raster attributes |
| APC (Kitty) | Kitty graphics protocol with chunked transmission |

---

## Configuration

Config is stored at `~/.config/ants-terminal/config.json` with **0600** file permissions.

```json
{
    "theme": "Dark",
    "font_size": 11,
    "opacity": 1.0,
    "background_alpha": 255,
    "scrollback_lines": 50000,
    "auto_copy_on_select": true,
    "session_logging": false,
    "background_blur": false,
    "gpu_rendering": false,
    "session_persistence": false,
    "image_paste_dir": "",
    "editor_command": "",
    "ai_endpoint": "",
    "ai_api_key": "",
    "ai_model": "llama3",
    "ai_context_lines": 50,
    "ai_enabled": false,
    "ssh_bookmarks": [],
    "plugin_dir": "",
    "enabled_plugins": [],
    "claude_project_dirs": [],
    "keybindings": {}
}
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `theme` | string | `"Dark"` | Active theme name |
| `font_size` | int | `11` | Font size in points (8-32) |
| `opacity` | double | `1.0` | Window opacity (0.1-1.0) |
| `background_alpha` | int | `255` | Per-pixel background alpha (0-255) |
| `scrollback_lines` | int | `50000` | Max scrollback lines (1000-1000000) |
| `auto_copy_on_select` | bool | `true` | Copy to clipboard on text selection |
| `session_logging` | bool | `false` | Log raw session to file |
| `background_blur` | bool | `false` | Enable KWin background blur |
| `gpu_rendering` | bool | `false` | Use OpenGL glyph atlas renderer |
| `session_persistence` | bool | `false` | Save/restore scrollback across restarts |
| `image_paste_dir` | string | `""` | Image paste save directory |
| `editor_command` | string | `""` | Editor for file path clicking |
| `ai_endpoint` | string | `""` | OpenAI-compatible API URL |
| `ai_api_key` | string | `""` | API key for AI endpoint |
| `ai_model` | string | `"llama3"` | LLM model to use |
| `ai_context_lines` | int | `50` | Lines of terminal output sent to AI |
| `ai_enabled` | bool | `false` | Enable AI assistant features |
| `ssh_bookmarks` | array | `[]` | Saved SSH connection bookmarks |
| `plugin_dir` | string | `""` | Lua plugin directory |
| `enabled_plugins` | array | `[]` | List of enabled plugin names |
| `claude_project_dirs` | array | `[]` | Directories for Claude Code projects |
| `keybindings` | object | `{}` | Custom keybindings (action -> key) |

---

## Plugins

### Creating a Plugin

1. Create a directory: `~/.config/ants-terminal/plugins/my-plugin/`
2. Create `init.lua`:

```lua
ants.log("My plugin loaded!")

ants.on("prompt", function(data)
    ants.notify("Prompt", "New command prompt")
end)
```

3. Optionally create `manifest.json`:

```json
{
    "name": "My Plugin",
    "version": "1.0.0",
    "description": "Does cool things",
    "author": "Your Name"
}
```

### Plugin API

| Function | Description |
|----------|-------------|
| `ants.on(event, callback)` | Register event handler |
| `ants.send(text)` | Send text to terminal PTY |
| `ants.notify(title, message)` | Show desktop notification |
| `ants.get_output(n)` | Get last N lines of output |
| `ants.get_cwd()` | Get current working directory |
| `ants.set_status(text)` | Set status bar text |
| `ants.log(message)` | Log message to status bar |

### Events

| Event | Data | Description |
|-------|------|-------------|
| `output` | Raw bytes | PTY data received |
| `line` | Line text | Complete line received |
| `prompt` | -- | OSC 133 prompt detected |
| `keypress` | Key name | Before key sent to PTY (return false to cancel) |
| `title_changed` | Title | Window title changed |
| `tab_created` | -- | New tab created |
| `tab_closed` | -- | Tab closed |

---

## Security

- **Config file permissions**: created with `0600` (owner read/write only)
- **No network access** by default -- AI assistant requires explicit configuration
- **OSC 52 clipboard**: write-only -- read disabled
- **Lua sandbox**: dangerous functions removed, 10M instruction timeout (pcall-proof), 10MB memory limit
- **SSH**: no password storage, interactive authentication only
- **AI**: API keys stored in 0600-permission config, 30-second network timeout
- **Session files**: bounds-validated on load, 100MB compressed size limit
- **Buffer limits**: CSI params capped at 32, images capped at 100+200, combining chars at 8
- **UTF-8 security**: overlong encodings, surrogates, out-of-range rejected
- **Bracketed paste**: prevents clipboard injection attacks

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
└── src/
    ├── main.cpp                # Entry point, OpenGL format setup
    ├── mainwindow.h/cpp        # Window, menus, themes, dialogs
    ├── titlebar.h/cpp          # Custom frameless title bar
    ├── terminalwidget.h/cpp    # Rendering, input, selection, search
    ├── terminalgrid.h/cpp      # Cell grid, scrollback, VtAction processing
    ├── vtparser.h/cpp          # VT100/xterm state machine, UTF-8 decoder
    ├── ptyhandler.h/cpp        # PTY via forkpty(), QSocketNotifier I/O
    ├── themes.h/cpp            # 7 color themes with ANSI palettes
    ├── config.h/cpp            # JSON config persistence (0600 perms)
    ├── commandpalette.h/cpp    # Searchable command palette overlay
    ├── glrenderer.h/cpp        # OpenGL glyph atlas + shader renderer
    ├── sessionmanager.h/cpp    # Session save/restore
    ├── aidialog.h/cpp          # AI assistant (OpenAI API)
    ├── sshdialog.h/cpp         # SSH bookmark manager
    ├── xcbpositiontracker.h/cpp # KWin window position tracking
    ├── claudeintegration.h/cpp # Claude Code status, hooks, MCP
    ├── claudeprojects.h/cpp    # Claude Code project/session browser
    ├── claudeallowlist.h/cpp   # Claude Code permission editor
    ├── claudetranscript.h/cpp  # Claude Code transcript viewer
    ├── luaengine.h/cpp         # Lua 5.4 scripting engine
    └── pluginmanager.h/cpp     # Plugin discovery + loading
```

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
