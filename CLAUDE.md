# Ants Terminal

A real terminal emulator built from scratch in C++ with Qt6.

## Tech Stack

- **C++20** with **Qt6** (Widgets, Core, Gui, Network, OpenGL, OpenGLWidgets)
- **Lua 5.4** (optional, for plugin system)
- **libutil** for PTY (forkpty)
- CMake build system

## Project Structure

```
├── CMakeLists.txt
├── src/
│   ├── main.cpp              # Entry point, OpenGL surface format setup
│   ├── mainwindow.h/.cpp     # Main window, menus, theme switching, dialog lifecycle
│   ├── terminalwidget.h/.cpp # QOpenGLWidget: renders grid, handles keyboard input
│   ├── terminalgrid.h/.cpp   # Character grid, scrollback, processes VtActions
│   ├── vtparser.h/.cpp       # VT100/xterm escape sequence state machine
│   ├── ptyhandler.h/.cpp     # PTY management (forkpty, read/write, resize)
│   ├── themes.h/.cpp         # 11 color themes with ANSI palette overrides
│   ├── config.h/.cpp         # Persistent JSON config (all settings)
│   ├── commandpalette.h/.cpp # Ctrl+Shift+P searchable command overlay
│   ├── glrenderer.h/.cpp     # OpenGL glyph atlas + shader-based renderer
│   ├── sessionmanager.h/.cpp # Session save/restore (scrollback serialization)
│   ├── aidialog.h/.cpp       # AI assistant dialog (OpenAI-compatible API)
│   ├── sshdialog.h/.cpp      # SSH manager dialog (bookmarks + connection)
│   ├── settingsdialog.h/.cpp # Settings dialog (all preferences)
│   ├── titlebar.h/.cpp       # Custom frameless title bar
│   ├── toggleswitch.h/.cpp   # Custom toggle switch widget
│   ├── auditdialog.h/.cpp    # Security audit checks dialog
│   ├── xcbpositiontracker.h/.cpp # KWin window position tracker
│   ├── claudeintegration.h/.cpp  # Claude Code process detection + hooks
│   ├── claudeallowlist.h/.cpp    # Claude Code permission rule editor
│   ├── claudeprojects.h/.cpp     # Claude Code project/session browser
│   ├── claudetranscript.h/.cpp   # Claude Code transcript viewer
│   ├── luaengine.h/.cpp      # Lua 5.4 scripting engine (sandboxed)
│   └── pluginmanager.h/.cpp  # Plugin discovery, loading, lifecycle
├── assets/                   # Icons
├── STANDARDS.md
├── RULES.md
└── README.md
```

## Architecture

Data flows: **PTY -> VtParser -> TerminalGrid -> TerminalWidget (QPainter or GlRenderer)**
Reverse flow: **TerminalGrid -> ResponseCallback -> PTY** (for DA, CPR, DSR)

- `Pty`: forkpty() spawns bash, QSocketNotifier for non-blocking reads
- `VtParser`: state machine decodes UTF-8, emits VtAction structs (Print, Execute, CSI, ESC, OSC, DCS, APC)
- `TerminalGrid`: processes VtActions, maintains cell grid + scrollback + cursor + modes
  - Mouse/focus/sync modes, OSC 8 hyperlinks, OSC 52 clipboard, OSC 133 shell integration
  - Kitty keyboard protocol (progressive enhancement with push/pop stack)
  - Desktop notifications (OSC 9/777), color palette notifications (CSI ? 2031)
  - Combining chars stored in per-line side table (`TermLine::combining`)
  - Response callback for DA1/DA2/CPR/DSR sequences
  - Sixel graphics (DCS), Kitty graphics (APC), iTerm2 images (OSC 1337)
  - Line reflow on resize (soft-wrapped lines re-wrapped to new width)
- `TerminalWidget` (inherits QOpenGLWidget): renders grid, handles keyboard/mouse -> PTY writes
  - Two render paths: QPainter (CPU) with QTextLayout ligatures, or GlRenderer (GPU)
  - SGR mouse reporting, focus reporting, synchronized output
  - Undercurl + colored underlines, font fallback, image paste auto-save
  - Per-pixel background alpha transparency
- `GlRenderer`: glyph atlas texture (2048x2048), GLSL 3.3 shaders, instanced quad rendering
- `MainWindow`: wires everything, manages themes/config/menus/keybindings/dialogs
- `AiDialog`: chat UI for OpenAI-compatible LLM API with streaming SSE
- `SshDialog`: SSH connection bookmark manager, connects via PTY ssh command
- `LuaEngine`: embedded Lua 5.4 with sandboxed `ants` API and event handlers
- `PluginManager`: discovers plugins in ~/.config/ants-terminal/plugins/, loads init.lua
- `SessionManager`: serializes scrollback + screen to compressed binary, saves/restores

## Building

```bash
mkdir build && cd build
cmake .. && make -j$(nproc)
./ants-terminal
```

### Optional Dependencies

- **Lua 5.4** (`lua54-devel`): enables plugin system. Without it, plugins are disabled at compile time.

## Conventions

- Signals/slots for cross-component communication
- Theme colors set on TerminalGrid (default fg/bg), used during rendering
- ANSI palette (16 standard + 216 cube + 24 gray) initialized in TerminalGrid
- Config persists to ~/.config/ants-terminal/config.json with 0600 permissions
- Scrollback configurable up to 1,000,000 lines (default 50,000)
- QTextLayout used for text rendering (enables ligature shaping via HarfBuzz)
- Lua plugins optional: gated behind `ANTS_LUA_PLUGINS` compile definition

## Key Design Decisions

- Custom VT100 parser (not pyte/libvterm) -- no external deps beyond Qt6
- QOpenGLWidget base class: GPU-accelerated QPainter by default, optional custom GL renderer
- QTextLayout for ligature-aware text shaping (HarfBuzz backend)
- Alt screen buffer support (for vim, htop, etc.)
- Delayed wrap (like xterm) for correct line-wrapping behavior
- SGR mouse reporting, focus reporting, synchronized output for TUI app compatibility
- Combining chars in per-line side table (zero memory overhead for lines without combiners)
- Image paste auto-saves to disk and inserts filepath (optimized for Claude Code workflow)
- Keybindings configurable via config.json
- GPU rendering via glyph atlas + GLSL shaders as optional accelerated path
- AI integration uses OpenAI-compatible API (works with Ollama, LM Studio, OpenAI, etc.)
- SSH manager uses system `ssh` binary via PTY (inherits user's SSH config/keys)
- Lua plugin sandbox removes dangerous globals, adds instruction count timeout
- Session persistence uses QDataStream + qCompress for efficient binary serialization
- Per-pixel transparency separate from window opacity (background alpha vs window alpha)

## Config Keys

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `theme` | string | `"Dark"` | Active theme name |
| `font_size` | int | `11` | Font size (8-32) |
| `opacity` | double | `1.0` | Window opacity (0.1-1.0) |
| `background_alpha` | int | `255` | Per-pixel bg alpha (0-255) |
| `scrollback_lines` | int | `50000` | Max scrollback (1K-1M) |
| `gpu_rendering` | bool | `false` | Use OpenGL glyph atlas renderer |
| `session_persistence` | bool | `false` | Save/restore scrollback on exit |
| `ai_endpoint` | string | `""` | OpenAI-compatible API URL |
| `ai_api_key` | string | `""` | API authentication key |
| `ai_model` | string | `"llama3"` | LLM model name |
| `ai_context_lines` | int | `50` | Terminal lines sent as context |
| `ai_enabled` | bool | `false` | Enable AI features |
| `ssh_bookmarks` | array | `[]` | Saved SSH connections |
| `plugin_dir` | string | `""` | Lua plugin directory |
| `enabled_plugins` | array | `[]` | Active plugin names |
| `snippets` | array | `[]` | Command snippets with placeholders |
| `auto_profile_rules` | array | `[]` | Auto-switch profiles by pattern |
| `badge_text` | string | `""` | Watermark text in terminal bg |
| `auto_color_scheme` | bool | `false` | Auto-switch dark/light theme |
| `dark_theme` | string | `"Dark"` | Theme for dark system mode |
| `light_theme` | string | `"Light"` | Theme for light system mode |
