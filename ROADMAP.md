# Ants Terminal — Roadmap

> **Current version:** 0.6.9 (2026-04-14). See [CHANGELOG.md](CHANGELOG.md)
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
2. [0.6.0 — shipped](#060--shipped-2026-04-14)
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

## 0.6.0 — shipped (2026-04-14)

**Theme of the release:** make the two features that already make Ants
distinctive (plugins + audit) production-grade. No sprawl; polish what's
there before adding a multiplexer. Reference:
[CHANGELOG.md §0.6.0](CHANGELOG.md#060--2026-04-14).

### 🎨 Features

- ✅ **Scrollback regex search**. `Ctrl+Shift+F` floating bar with
  three toggles — `.*` (regex), `Aa` (case), `Ab` (whole word,
  Alt+R/C/W) — plus match counter, next/prev jump, all-match
  highlight overlay, current-match accent, invalid-pattern feedback.
  Ghostty 1.3 parity.
- ✅ **OSC 9;4 progress reporting**. Parses
  `ESC ] 9 ; 4 ; state ; pct ST` (ConEmu / Microsoft Terminal), renders
  a thin colored strip along the bottom edge plus a tab-icon dot.
- ✅ **Click-to-move-cursor on shell prompts** (previously in code;
  docs promoted).
- 💭 **Kitty Unicode-placeholder graphics** (U+10EEEE + diacritics).
  Moved to 0.7 backlog — scope-wise a standalone item, not a blocker.

### 🔌 Plugins — manifest v2 + capability model

- ✅ **Declarative permissions in `manifest.json`** — `permissions`
  array with first-load confirmation dialog, persisted per-plugin grants
  in `config.json` (`plugin_grants`). Un-granted permissions = missing
  API functions (not `nil` stubs) so plugins can feature-detect.
- ✅ **Per-plugin Lua VMs** — each plugin gets its own `lua_State`,
  10 MB heap budget, 10 M instruction hook. `PluginManager` fans
  events out over all loaded VMs.
- ✅ **`ants.settings.get / .set`** — gated by `"settings"`. Backed by
  `config.json`'s `plugin_settings.<name>.<key>`. Settings-dialog
  auto-render of `"settings_schema"` is follow-up (API + store in
  place).
- ✅ **`ants.clipboard.write(text)`** — gated by `"clipboard.write"`.
- ✅ **Hot reload** via `QFileSystemWatcher` on `init.lua` /
  `manifest.json` / plugin dir. Fires `load` / `unload` lifecycle
  events. Enabled only when `ANTS_PLUGIN_DEV=1`.
- ✅ **Plugin keybindings** — `manifest.json` `"keybindings":
  {"action-id": "Ctrl+Shift+X"}` block. Firing the shortcut sends a
  `keybinding` event to the owning plugin.
- ✅ **`ants._version`** + **`ants._plugin_name`** exposed.

### ⚡ Performance

- ✅ **LRU glyph-atlas eviction**. Per-glyph `lastFrame` counter,
  warm-half retention on overflow, median-based fallback. See
  `glrenderer.cpp:compactAtlas`.
- ✅ **Per-line dirty tracking** (partial). `TermLine::dirty` set by
  grid mutations; `TerminalWidget::invalidateSpanCaches()` does
  targeted eviction of URL / highlight caches. Full cell-level
  render-path partial-update remains as future work — primitive is in
  place.
- 💭 **EGL_EXT_swap_buffers_with_damage + EGL_EXT_buffer_age** on the GL
  path. Deferred — narrow Mesa-only win, revisit in 0.9 platform pass.

### 🔒 Security

- ✅ **Multi-line paste confirmation**. Dialog on `\n` / `sudo ` /
  `curl … | sh` / control chars. Policy independent of bracketed-paste
  mode. Config key `confirm_multiline_paste` (default on).
- ✅ **Per-terminal OSC 52 write quota**: 32 writes/min + 1 MB/min
  (independent of the 1 MB per-string cap).
- ✅ **OSC 8 homograph warning**: when visible label's hostname ≠
  URL host, a confirm dialog shows both and requires opt-in.

### 🧰 Dev experience

- ✅ **`ants-terminal --new-plugin <name>`** — scaffolds init.lua /
  manifest.json / README.md from templates. Name validated against
  `[A-Za-z][A-Za-z0-9_-]{0,63}`.
- ✅ **`ANTS_PLUGIN_DEV=1`** — verbose logging on plugin scan/load,
  plus hot reload.
- ✅ **`--help` / `--version`** CLI flags wired via
  `QCommandLineParser`.

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

- ✅ **Trigger rules** with `instant` flag and new action types
  (`bell`, `inject`, `run_script` alongside the existing `notify` /
  `sound` / `command`). Shipped in 0.6.9. The `HighlightLine` /
  `HighlightText` / `MakeHyperlink` actions from the iTerm2 reference
  are deferred — they need grid-cell mutation surgery and are tracked
  as a follow-up; everything else from the iTerm2 trigger doc has
  parity. See [CHANGELOG.md §0.6.9](CHANGELOG.md#069--2026-04-14).
- ✅ **User-vars channel**: OSC 1337 SetUserVar parsing + the
  `user_var_changed` event. Shipped in 0.6.9. Disambiguated from
  inline images by the byte after `1337;`. NAME ≤ 128 chars; decoded
  value capped at 4 KiB. See
  [CHANGELOG.md §0.6.9](CHANGELOG.md#069--2026-04-14).
- ✅ **Command-palette registration**: `ants.palette.register({title,
  action, hotkey})` shipped in 0.6.9. Always-on (no permission gate),
  optional global QShortcut. Entry triggers fire scoped
  `palette_action` event. See
  [CHANGELOG.md §0.6.9](CHANGELOG.md#069--2026-04-14).
- ✅ Events: `command_finished` (exit + duration), `pane_focused`,
  `theme_changed`, `window_config_reloaded`, `user_var_changed`,
  `palette_action`. All shipped in 0.6.9. See
  [CHANGELOG.md §0.6.9](CHANGELOG.md#069--2026-04-14).

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

### 🧰 Dev experience — Project Audit tool

Surfaced by the 0.6.5 audit pass (see [CHANGELOG.md §0.6.5](CHANGELOG.md#065--2026-04-14)).
Each item is a rule / self-check the in-app `AuditDialog` should grow so
findings like the ones we caught manually would be caught automatically on
the next run.

- ✅ **Qt rule: unbounded callback payloads.** Shipped in 0.6.8 as
  `unbounded_callback_payloads` — same-line regex flags
  `QString::fromUtf8(<expr>.c_str()…)` forwarded to a `*Callback(…)`
  without a `.left()` / `.truncate()` / `.mid()` / `.chopped()` /
  `.chop()` cap. See [CHANGELOG.md §0.6.8](CHANGELOG.md#068--2026-04-14).
- ✅ **Qt rule: `QNetworkReply` connects without context.** Shipped in
  0.6.8 as `qnetworkreply_no_abort` — flags 3-arg `connect()` to
  QNetworkReply signals whose third argument is a bare lambda (no
  context object means no auto-disconnect on owner destruction → UAF
  if reply fires after `this` is gone). The 4-arg form is enforced.
  See [CHANGELOG.md §0.6.8](CHANGELOG.md#068--2026-04-14).
- ✅ **Observability rule: silent `catch (...)`.** Shipped in 0.6.7 as
  `silent_catch` — flags empty same-line `catch(...) {}` handlers.
  Extending to multi-line / single-statement trivial bodies remains future
  work (`grep -Pzo` plumbing). See [CHANGELOG.md §0.6.7](CHANGELOG.md#067--2026-04-14).
- ✅ **Self-consistency: fixture-per-`addGrepCheck`.** Shipped in 0.6.6 —
  `audit_fixture_coverage` rule in the dialog + CI-enforced cross-check in
  `tests/audit_self_test.sh`. See [CHANGELOG.md §0.6.6](CHANGELOG.md#066--2026-04-14).
- ✅ **Build-flag recommender.** Shipped in 0.6.7 as `missing_build_flags`
  — scans the top-level `CMakeLists.txt` for `-Wformat=2`, `-Wshadow`,
  `-Wnull-dereference`, `-Wconversion`. See
  [CHANGELOG.md §0.6.7](CHANGELOG.md#067--2026-04-14).
- ✅ **No-CI check.** Shipped in 0.6.7 as `no_ci` — covers `.github/workflows/`,
  `.gitlab-ci.yml`, `.circleci/`, `.travis.yml`, and `Jenkinsfile`. See
  [CHANGELOG.md §0.6.7](CHANGELOG.md#067--2026-04-14).
- ✅ **Sanitizer-in-ctest hookup.** Shipped in 0.6.7 — dedicated `build-asan`
  job in `.github/workflows/ci.yml` runs ctest + a binary smoke test under
  ASan/UBSan with `QT_QPA_PLATFORM=offscreen` on every push. See
  [CHANGELOG.md §0.6.7](CHANGELOG.md#067--2026-04-14).
- ✅ **`CONTRIBUTING.md`.** Shipped in 0.6.7 — derived from `STANDARDS.md`,
  covers build modes, test layout, adding an audit rule, version-bump
  checklist. See [CHANGELOG.md §0.6.7](CHANGELOG.md#067--2026-04-14).

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
