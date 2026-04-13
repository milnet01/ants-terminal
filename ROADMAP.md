# Ants Terminal — Roadmap

> **Current version:** 0.5.0 (2026-04-13). See [CHANGELOG.md](CHANGELOG.md)
> for what's shipped; see [PLUGINS.md](PLUGINS.md) for plugin-author
> standards; this document covers what's **planned**.

This roadmap is organized by **target release**, and within each release by
**theme**. Every item has prior art and a one-line implementation note so
it's actionable — not aspirational. Dates are targets, not commitments;
they move based on contributor bandwidth and real-world usage feedback.

**Legend**
- ✅ Done (shipped)
- 🚧 In progress (branch or open PR)
- 📋 Planned (next up for this release)
- 💭 Considered (research phase; may change scope or slip)

**Themes**
- 🎨 **Features** — user-visible capabilities
- ⚡ **Performance** — runtime cost reductions
- 🔌 **Plugins** — extensibility surface
- 🖥 **Platform** — packaging, ports, accessibility
- 🔒 **Security** — attack-surface reductions
- 🧰 **Dev experience** — tooling, testing, docs

---

## Table of Contents

1. [0.5.0 — shipped](#050--shipped-2026-04-13)
2. [0.6.0 — context polish + plugin v2](#060--context-polish--plugin-v2-target-2026-05)
3. [0.7.0 — shell integration + triggers](#070--shell-integration--triggers-target-2026-06)
4. [0.8.0 — multiplexing + marketplace](#080--multiplexing--marketplace-target-2026-08)
5. [0.9.0 — platform + a11y](#090--platform--a11y-target-2026-10)
6. [1.0.0 — stability milestone](#100--stability-milestone-target-2026-12)
7. [Beyond 1.0 — long-horizon](#beyond-10--long-horizon)
8. [How to propose a roadmap item](#how-to-propose-a-roadmap-item)

---

## 0.5.0 — shipped (2026-04-13)

Reference point — see [CHANGELOG.md](CHANGELOG.md#050--2026-04-13) for the
full list. Headline: **context-aware Project Audit** (inline suppressions,
generated-file skip, path rules, code snippets, git blame enrichment,
confidence score 0–100, Semgrep integration, AI triage per finding,
version stamping on every export).

---

## 0.6.0 — context polish + plugin v2 (target: 2026-05)

**Theme of the release:** make the two features that already make Ants
distinctive (plugins + audit) production-grade. No sprawl; polish what's
there before adding a multiplexer.

### 🎨 Features

- 📋 **Scrollback regex search with replace-all highlight**. Ghostty 1.3
  shipped this as its #1 most-requested feature
  ([notes](https://ghostty.org/docs/install/release-notes/1-3-0)). Wire
  `Ctrl+Shift+F` → floating bar, regex toggle, case toggle, match count,
  next/prev jump, all-match highlight overlay.
- 📋 **OSC 9;4 progress reporting**. Parse `ESC ] 9 ; 4 ; state ; pct ST`
  and render a thin progress strip on the tab. Shells already emit it
  ([Microsoft docs](https://learn.microsoft.com/en-us/windows/terminal/tutorials/progress-bar-sequences)).
- 📋 **Click-to-move-cursor on shell prompts**. When OSC 133 says the
  cursor is at a prompt, clicking elsewhere on that line sends the
  correct arrow-key count. Ghostty 1.3 feature.
- 💭 **Kitty Unicode-placeholder graphics** (U+10EEEE + diacritics). Makes
  images survive `tmux`; small spec, big compat win
  ([PR #5664](https://github.com/kovidgoyal/kitty/pull/5664)).

### 🔌 Plugins — manifest v2 + capability model

This is the big plugin-system expansion. See [PLUGINS.md →
Roadmap](PLUGINS.md#06--capability-manifest--clipboard--settings-schema)
for the author-facing contract.

- 📋 **Declarative permissions in `manifest.json`** — borrow the browser-
  extension model: `permissions: ["read_output", "write_pty",
  "clipboard.write", "fs:~/.local/share/myplugin", "net:https://api.x.com"]`.
  Un-granted permissions surface as `nil` in the Lua env. First launch
  shows the list and asks the user to accept.
- 📋 **Per-plugin Lua VMs**. Today plugins share one `lua_State`, which
  leaks globals. Give each plugin its own VM with its own instruction /
  heap budget, so one misbehaving plugin can't destabilize others.
- 📋 **`ants.settings.get(key)` / `ants.settings.set(key, value)`** with
  a JSON-Schema `"settings_schema"` in `manifest.json`; Settings dialog
  auto-renders a tab for plugins that declare one (Tabby-style
  ConfigProvider [docs](https://deepwiki.com/Eugeny/tabby/9.2-plugin-development)).
- 📋 **`ants.clipboard.write(text)`** gated by `clipboard.write`.
- 📋 **Hot reload** via `QFileSystemWatcher` on plugin dirs. Adds
  `on_load` / `on_unload` lifecycle hooks.
- 📋 **Plugin keybinding registration** via `"keybindings"` manifest
  block. Resolves conflicts against the global keymap at load time.
- 📋 **`ants._version`** exposes the terminal version as a Lua string
  so plugins can feature-detect without hardcoding.

### ⚡ Performance

- 📋 **LRU glyph-atlas eviction**. Today we clear and rebuild on overflow;
  switch to per-glyph `last_used_frame` counter, evict oldest 10% when
  full. Eliminates the redraw stutter when a long-running ligature-heavy
  session cycles through glyphs.
- 📋 **Cell-level dirty tracking**. Add `dirty: bool` to `Cell` (or a
  per-row bitmap). Clear after render. Repaint cost drops from
  O(rows×cols) to O(changed cells). Ghostty + foot both use this
  approach ([HN thread](https://news.ycombinator.com/item?id=42518110)).
- 💭 **EGL_EXT_swap_buffers_with_damage + EGL_EXT_buffer_age** on the GL
  path. Mesa has shipped both since ~2014. Reported swing: 60W → 50W GPU
  draw on partial-frame updates.

### 🔒 Security

- 📋 **Multi-line paste confirmation**. Dialog appears when clipboard
  contains `\n`, `sudo `, `curl … | sh`, or control chars. iTerm2,
  Tabby, Microsoft Terminal all do this
  ([Tabby #2131](https://github.com/Eugeny/tabby/issues/2131)).
  Important: policy independent of bracketed-paste — Microsoft Terminal
  #13014 caught flak for turning the warning off when bracketed-paste
  was active.
- 📋 **Per-host OSC 52 write quota**: 32 writes/min + 1 MB/min per
  terminal, independent of the existing per-string cap.
- 📋 **OSC 8 homograph warning**: when the visible link text differs
  from the URL host, show a confirm dialog (browser-style).

### 🧰 Dev experience

- 📋 **Plugin scaffolding command**: `ants-terminal --new-plugin my-plugin`
  creates a directory with `init.lua`, `manifest.json`, and a `README.md`
  template.
- 📋 **Plugin dev mode**: env var `ANTS_PLUGIN_DEV=1` enables verbose
  logging + hot reload for in-development plugins.

---

## 0.7.0 — shell integration + triggers (target: 2026-06)

**Theme:** make Ants the best terminal for **shell-aware workflows**.

### 🎨 Features — shell integration beyond OSC 133

- 📋 **Command blocks as first-class UI** (Warp's headline feature,
  [docs](https://docs.warp.dev/terminal/blocks)). OSC 133 markers
  already give us the data; wire the UI:
  - Collapsible prompt → command → output groups
  - `Cmd/Ctrl+Up` / `Cmd/Ctrl+Down` jump to previous/next prompt
  - Per-block menu: copy command, copy output, re-run, share as
    `.cast`, suppress output noise
  - Exit code + duration + CWD as block metadata
- 📋 **Asciinema recording** (`.cast` v2 format,
  [spec](https://docs.asciinema.org/manual/asciicast/v2/)). Single JSON
  header line + event lines. Menu entry `File → Record session`.
- 💭 **Semantic history**: Cmd-click on `path:line:col` in output opens
  the file in the configured IDE (`vscode://file/{path}:{line}`, etc.).
  Works by scraping scrollback; no shell cooperation needed.
- 💭 **Shell-side HMAC verification** for OSC 133 markers — protects
  the command-block UI from forged markers written by an inner TTY
  process. Key passed via `ANTS_OSC133_KEY` env; shell hook computes
  HMAC over `(prompt_id, command, exit_code)`.

### 🔌 Plugins — trigger system

- 📋 **Trigger rules** as config: `{regex, action, params, instant}`
  evaluated per output line. Actions: `HighlightLine`, `HighlightText`,
  `MakeHyperlink`, `InjectText`, `RingBell`, `PostNotification`,
  `RunScript` (Lua function by name)
  ([iTerm2 docs](https://iterm2.com/documentation-triggers.html)).
  "Instant" triggers evaluate before the line is complete — for
  password prompts.
- 📋 **User-vars channel**: parse `ESC ] 1337 ; SetUserVar=NAME=<b64> ST`
  and fire `user_var_changed` event. Lets a shell plugin pipe state
  (git branch, k8s context, virtualenv) to plugins without prompt
  parsing ([WezTerm docs](https://wezterm.org/recipes/passing-data.html)).
- 📋 **Command-palette registration**: `ants.palette.register({
  title, action, hotkey})`. Mirrors WezTerm's
  `augment-command-palette` event.
- 📋 Events: `command_finished` (exit + duration), `pane_focused`,
  `theme_changed`, `window_config_reloaded`.

### ⚡ Performance

- 📋 **SIMD VT-parser scan**. Ghostty's read thread scans for `0x1B`,
  `0x07`, `0x9B` via CPU SIMD before dispatching to the state machine.
  Dominant parser speedup in Ghostty's benchmark
  ([repo](https://github.com/ghostty-org/ghostty)).
- 📋 **Decouple read/parse/render thread**. Today everything's on the
  Qt GUI thread. Move PTY read + VT parse to a worker; push `VtAction`
  diffs over a lock-free ring to the render path.
- 💭 **Incremental reflow on resize**. Track `wrap_col` per line;
  skip re-wrapping lines whose width is still ≤ `wrap_col` under
  the new width. O(scrollback) pause on window-drag disappears.

### 🖥 Platform

- 📋 **Wayland-native Quake-mode**: implement a layer-shell-based
  dropdown via `wlr-layer-shell-unstable-v1`; global hotkey via
  KGlobalAccel on KDE, dbus on GNOME. XCB path stays for X11.

### 🔒 Security

- 📋 **Plugin capability audit UI**: Settings → Plugins → `<name>`
  shows the full permission list, with a toggle to revoke individual
  capabilities. Revocation takes effect at next plugin reload.
- 📋 **Image-bomb defenses**: cap decoded sixel/Kitty/iTerm2 dimensions
  at 16384×16384 and total in-memory image bytes per terminal at
  256 MB. Reject with an inline error.

---

## 0.8.0 — multiplexing + marketplace (target: 2026-08)

**Theme:** big new capabilities. This is the "features you'd expect from
a modern terminal" release.

### 🎨 Features — multiplexing

- 📋 **Headless mux server with codec RPC**. WezTerm's architecture
  ([DeepWiki](https://deepwiki.com/wezterm/wezterm/2.2-multiplexer-architecture)):
  `ants-terminal --server` runs without a GUI and accepts attachments
  over a Unix socket; `ants-terminal --attach <socket>` reconnects.
  Panes survive window close. Sparse scrollback fetched on demand via
  `GetLines` RPC.
- 📋 **Remote-control protocol** (Kitty-style,
  [docs](https://sw.kovidgoyal.net/kitty/rc_protocol/)): JSON envelopes
  over a Unix socket. Commands: `launch`, `send-text`, `set-title`,
  `select-window`, `get-text`, `ls`, `new-tab`. Auth via X25519 when
  a password is set. Unlocks scripting, IDE integration, CI.
- 📋 **SSH ControlMaster** auto-integration from the SSH bookmark
  dialog: `-o ControlMaster=auto -o ControlPath=~/.ssh/cm-%r@%h:%p
  -o ControlPersist=10m`. Second tab to the same host opens in ms
  instead of seconds.
- 💭 **Domain abstraction** à la WezTerm: `DockerDomain` lists
  `docker ps`, opens a tab via `docker exec -it`; `KubeDomain` lists
  pods, opens via `kubectl exec`. Reuses the SSH bookmark UI shell.
- 💭 **Persistent workspaces**: save/restore entire tab+split layout +
  scrollback to disk; one-click "resume yesterday's dev session."

### 🔌 Plugins — marketplace

- 📋 **Signed plugin packaging**: Ed25519 sig over a tarball containing
  `init.lua`, `manifest.json`, and optional assets. Loader verifies
  against a project-maintained keyring + (optionally) user-added keys.
- 📋 **Public marketplace index**: static JSON hosted on GitHub Pages
  listing name, version, author, signature-status, permission summary.
  Settings → Plugins → Browse lists them with an install button.
- 📋 **Plugin dependency resolution**: `manifest.json` `requires: [...]`
  field; install flow resolves transitively.

### ⚡ Performance

- 📋 **Dynamic grid storage** (Alacritty
  [PR #1584](https://github.com/alacritty/alacritty/pull/1584/files)).
  Don't pre-allocate the full `Vec<Vec<Cell>>` scrollback; lazily
  allocate row buffers; intern empty rows to a single shared sentinel.
  Alacritty's own data: 191 MB → 34 MB (20k-line scrollback).
- 📋 **Async image decoding**. Hand sixel/Kitty/iTerm2 payloads to
  `QtConcurrent::run`; render a placeholder cell until `QImage`
  future resolves. Big sixel frames stop blocking the prompt.
- 💭 **BTree scrollback** — O(log n) scroll-to-line instead of O(n)
  for jump-to-timestamp features.

### 🖥 Platform

- 📋 **Flatpak packaging**. Ship `org.ants.Terminal.yml` against
  `org.kde.Platform//6.7`. PTY inside Flatpak needs `flatpak-spawn
  --host` — Ghostty precedent.

---

## 0.9.0 — platform + a11y (target: 2026-10)

**Theme:** reach new users. Port, accessibility, internationalization.

### 🖥 Platform

- 📋 **macOS port**. Qt6 ports cleanly; replace `forkpty` with
  `posix_spawn` + `openpty`, swap `xcbpositiontracker` for
  `NSWindow` KVO observers, sign+notarize the `.app`.
- 💭 **Windows port**. ConPTY via `CreatePseudoConsole` replaces
  PTY; `xcbpositiontracker` becomes a no-op. Qt6's Windows platform
  plugin handles the rest.

### 🖥 Accessibility

- 📋 **AT-SPI/ATK support**. Qt6 has AT-SPI over D-Bus natively. Work:
  implement `QAccessibleInterface` for `TerminalWidget` exposing
  role `Terminal`; fire `text-changed` / `text-inserted` on grid
  mutations (gate on OSC 133 `D` markers to batch); expose cursor as
  caret ([freedesktop AT-SPI2](https://www.freedesktop.org/wiki/Accessibility/AT-SPI2/)).
  Without this, Orca reads nothing in the terminal.
- 💭 **Screen-magnifier-friendly rendering**: honor
  `QGuiApplication::styleHints()->mousePressAndHoldInterval()` and
  provide high-contrast theme variants.

### 🌍 Internationalization

- 📋 **i18n scaffolding**. Qt's `lupdate` / `linguist` flow; wrap all
  UI strings with `tr()`; ship `.qm` files in `assets/i18n/`. Today
  we have zero `tr()` usage. Start with English → Spanish, French,
  German as a proof of concept.
- 💭 **Right-to-left text support** — bidirectional text in the grid.
  Non-trivial; defer until demand is concrete.

### 🧰 Dev experience

- 📋 **Plugin development SDK**: `ants-terminal --plugin-test <dir>`
  runs a plugin against a mock PTY with scripted events. Enables
  unit-testing plugins.

---

## 1.0.0 — stability milestone (target: 2026-12)

**Theme:** API freeze. No new features; quality, docs, migration guide.

- 📋 **`ants.*` API stability pledge**: the 1.0 surface won't break in
  `1.x` minor releases. Breaking changes queue for 2.0.
- 📋 **Performance regression suite**: CI benchmarks (grid throughput,
  scrollback allocation, paint-loop time) with commit-level deltas.
- 📋 **Documentation pass**: every user-facing feature has at least one
  screenshot + one animated demo.
- 📋 **Security audit + disclosure policy**. `SECURITY.md` with
  coordinated-disclosure process; in-place response to any CVE-class
  issue within 30 days.
- 📋 **Plugin migration guide** for any manifest/API changes between
  0.9 and 1.0.

---

## Beyond 1.0 — long-horizon

These are far enough out that specifics will change. Captured here so
contributors don't duplicate research.

### 🔌 Plugins

- 💭 **WebAssembly plugins** via `wasmtime` or `wasmer`. Same `ants.*`
  API exposed as WASI imports. Lua plugins continue to work — WASM is
  additive for authors who want Rust/Go/AssemblyScript. Stronger
  sandbox than Lua's removed-globals model; language-agnostic. Ghostty
  is experimenting with a WASM-targeting VT library today.
- 💭 **Inter-plugin pub/sub**: `ants.bus.publish(topic, data)` /
  `ants.bus.subscribe(topic, handler)`. Needs careful permission
  modeling — a "read_bus: <topic>" capability.

### 🎨 Features

- 💭 **AI command composer** (Warp-style). Dialog over the prompt
  accepts natural language, returns a shell command + explanation.
  Uses the existing OpenAI-compatible config; opt-in per invocation.
- 💭 **Collaborative sessions**: real-time shared terminal with a
  second user via an end-to-end encrypted relay. The "share
  terminal with a colleague" feature tmate popularized.
- 💭 **Workspace sync**: mirror `config.json`, plugins, and SSH
  bookmarks across devices via a user-configurable git remote.

### 🔒 Security

- 💭 **Confidential computing**: run the PTY in an SGX/SEV enclave,
  with the renderer as the untrusted host. Meaningful for people who
  type secrets into the terminal — every keystroke lives only in
  enclave memory until it's shown on-screen. Heavy lift; benefit
  concentrated in a small user set.

### ⚡ Performance

- 💭 **GPU text rendering with ligatures**. Today GPU path can't
  render ligatures (HarfBuzz shaping is on the CPU path). Port the
  shaping step to a compute shader; keep the atlas path we already
  have.

---

## How to propose a roadmap item

Open a GitHub issue with:

1. **What**: one-sentence description of the capability or change.
2. **Why**: concrete user problem it addresses. Link to the source
   (forum post, issue, personal story) if possible.
3. **Prior art**: has another terminal solved this? Which one, how?
   Link the source.
4. **Scope hint**: is this a weekend change, a week, or a month?
5. **Category**: which theme (🎨/⚡/🔌/🖥/🔒/🧰) does it belong to?

Items that fit naturally into the current release arc land in that
release. Items that don't get queued here with a 💭 marker until
the scope clarifies.

**A roadmap item is not a commitment.** It's a record that we thought
about it, found the prior art, and believe it fits the product. Scope
changes with bandwidth and real-world feedback. Check the
[CHANGELOG.md](CHANGELOG.md) for what actually shipped.
