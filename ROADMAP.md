# Ants Terminal — Roadmap

> **Current version:** 0.6.37 (2026-04-18) (2026-04-18). See [CHANGELOG.md](CHANGELOG.md)
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
- 🖥 **Platform** — ports (macOS, Windows), Wayland native support, a11y
- 🔒 **Security** — attack-surface reductions
- 🧰 **Dev experience** — tooling, testing, docs
- 📦 **Packaging & distribution** — the work of getting Ants into distros
  as an installable, auditable, default-able terminal. See the
  [distribution-adoption overview](#distribution-adoption-overview) for
  the full multi-release plan.

---

## Distribution-adoption overview

Becoming a distribution's **default** terminal is a long game — every
mainstream default is built by the desktop-environment team itself
(Konsole/KDE, gnome-terminal/GNOME). Outside projects like Ghostty,
WezTerm, Kitty, Alacritty are widely **packaged** but not default
anywhere. Our realistic target is: **get packaged everywhere, accrue
the quality signals distros look for, and let small/niche distros
adopt first.** The work is tracked below under the 📦 theme in each
release; this section is the rollup so nothing falls by the wayside.

| Bundle | What ships | Status | Target release |
|--------|------------|--------|----------------|
| **H1** | `SECURITY.md` coordinated-disclosure policy, `CODE_OF_CONDUCT.md` (Contributor Covenant 2.1) | ✅ | 0.6.16 |
| **H2** | AppStream `org.ants.Terminal.metainfo.xml`, polished desktop entry (`org.ants.Terminal.desktop`), icon install rules | ✅ | 0.6.17 |
| **H3** | Man page `ants-terminal.1` + CMake install rule | ✅ | 0.6.18 |
| **H4** | Bash / zsh / fish completions + CMake install rules | ✅ | 0.6.19 |
| **H5** | openSUSE OBS `.spec`, Arch AUR `PKGBUILD`, Debian `debian/` tree — ready-to-submit packaging files committed to tree | ✅ | 0.6.20 |
| **H6** | Flatpak manifest (`org.ants.Terminal.yml`) + Flathub submission | 📋 | 0.8.0 |
| **H7** | Project website + docs site (GitHub Pages) with screenshots, getting-started, plugin authoring | 📋 | 0.8.0 |
| **H8** | macOS port (Qt6 builds cleanly; need `posix_spawn`/`openpty` + NSWindow observer + notarized `.app`) | 📋 | 0.9.0 |
| **H9** | AT-SPI / ATK accessibility — `QAccessibleInterface` for `TerminalWidget`, `text-changed` events batched on OSC 133 D | 📋 | 0.9.0 |
| **H10** | i18n scaffolding — `lupdate`/`linguist` flow, `tr()` on UI strings, `.qm` files for en/es/fr/de | 📋 | 0.9.0 |
| **H11** | Reproducible-build verification + SBOM (`cmake --build` under `SOURCE_DATE_EPOCH`; generate SPDX SBOM) | 💭 | 0.9.0 |
| **H12** | Windows port (ConPTY + Qt6 Windows plugin; sign + MSI) | 💭 | Beyond 1.0 |
| **H13** | Distro-outreach pitch: file "intent-to-package" bugs in Debian / Fedora / NixOS; write launch post for r/linux, Hacker News, Phoronix tip line, LWN | 📋 | 0.8.0 |
| **H14** | Grow bus factor ≥ 2: second maintainer with commit rights, documented governance | 💭 | 1.0.0+ |
| **H15** | FOSDEM lightning talk / devroom slot (Linux desktop devroom is where distro maintainers converge) | 💭 | Beyond 1.0 |
| **H16** | Sponsorship / funding: GitHub Sponsors, Open Collective, or similar | 💭 | Beyond 1.0 |

Already shipped (as of this ROADMAP revision):

- ✅ **`.desktop` file** — pre-0.5 ancestor preserved as
  `ants-terminal.desktop.in` for the dev-symlink workflow; 0.6.17
  adds a spec-compliant `packaging/linux/org.ants.Terminal.desktop`
  for distro installs
- ✅ **Icons at multiple sizes** — `assets/ants-terminal-{16,32,48,64,128,256}.png`,
  installed under `share/icons/hicolor/<size>x<size>/apps/ants-terminal.png`
  by 0.6.17's CMake rules
- ✅ **CHANGELOG.md + SemVer discipline** — every release has a dated section with categorical bullets
- ✅ **CI with ASan + UBSan** — the `build-asan` lane runs sanitized ctest + binary smoke on every push
- ✅ **Audit pipeline** — clazy + cppcheck + grep rules + fixture-enforced regression coverage

Gating items (blocks adoption **today**, not just for being default):

1. **No distro packages anywhere** — H5 + H6 unblock this.
2. ~~**No AppStream metadata** — H2 unblocks GNOME Software / KDE Discover discovery.~~ ✅ Shipped in 0.6.17.
3. ~~**No SECURITY.md** — H1. Distro security teams need a disclosure contact before shipping.~~ ✅ Shipped in 0.6.16.
4. **Linux-only + X11-only dropdown** — H8 (macOS) and §0.7.0 Wayland-native Quake widen the addressable audience.

---

## Table of Contents

1. [Distribution-adoption overview](#distribution-adoption-overview)
2. [0.5.0 — shipped](#050--shipped-2026-04-13)
3. [0.6.0 — shipped](#060--shipped-2026-04-14)
4. [0.7.0 — shell integration + triggers](#070--shell-integration--triggers-target-2026-06)
5. [0.8.0 — multiplexing + marketplace](#080--multiplexing--marketplace-target-2026-08)
6. [0.9.0 — platform + a11y](#090--platform--a11y-target-2026-10)
7. [1.0.0 — stability milestone](#100--stability-milestone-target-2026-12)
8. [Beyond 1.0 — long-horizon](#beyond-10--long-horizon)
9. [How to propose a roadmap item](#how-to-propose-a-roadmap-item)

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

- ✅ **Command blocks as first-class UI** (Warp parity,
  [docs](https://docs.warp.dev/terminal/blocks)). Shipped across
  several releases and consolidated in 0.6.10. Prompt → command →
  output grouping via `OSC 133` markers, `Ctrl+Shift+Up` /
  `Ctrl+Shift+Down` jump-to-prev/next prompt, collapsible output with
  "… N lines hidden" summary bar, duration + timestamp in the prompt
  gutter, 2px pass/fail status stripe, and per-block right-click
  menu: Copy Command, Copy Output, Re-run Command, Fold/Unfold,
  Share Block as `.cast`. "Suppress output noise" is deferred — the
  original sub-bullet didn't define a noise heuristic and wasn't
  worth blocking the bundle. See
  [CHANGELOG.md §0.6.10](CHANGELOG.md#0610--2026-04-14).
- ✅ **Asciinema recording** (`.cast` v2 format,
  [spec](https://docs.asciinema.org/manual/asciicast/v2/)). Shipped.
  Full-session recording via `Settings → Record Session` (default
  `Ctrl+Shift+R`); per-block export via the command-block context
  menu in 0.6.10. Asciicast v2 header + event-stream format. See
  [CHANGELOG.md §0.6.10](CHANGELOG.md#0610--2026-04-14).
- ✅ **Semantic history**. Shipped in 0.6.x and broadened in 0.6.12.
  Ctrl-click on a `path:line:col` capture in scrollback (compiler /
  linter / stack-trace output) opens the file at the cited line/col
  via `TerminalWidget::openFileAtPath()`. Detection lives in
  `detectUrls()` (regex captures `path:line[:col]`); CWD resolution
  uses `/proc/<pid>/cwd` so relative paths Just Work without shell
  cooperation. 0.6.12 broadened the editor switch from VS Code + Kate
  only to also cover the VS Code family (`code-insiders`, `codium`),
  vi-family (`nvim`, `vim`), `nano`, Sublime / Helix / Micro
  (`path:line:col` argv shape), and JetBrains IDEs (`--line N
  --column M`). See
  [CHANGELOG.md §0.6.12](CHANGELOG.md#0612--2026-04-14).
- ✅ **Shell-side HMAC verification** for OSC 133 markers. Shipped in
  0.6.31. When `$ANTS_OSC133_KEY` is set in the terminal's environment,
  every OSC 133 marker (`A`/`B`/`C`/`D`) must carry an `ahmac=` param
  computed as HMAC-SHA256(key, `<marker>|<promptId>[|<exitCode>]`).
  Markers without a valid HMAC are dropped and a forgery counter
  increments, surfaced as a status-bar warning with a 5-second
  cooldown. Bash + zsh hook scripts ship under
  `packaging/shell-integration/`. Headless feature test
  (`tests/features/osc133_hmac_verification/`) covers verifier OFF
  back-compatibility, verifier ON accept of valid HMACs (including
  uppercase-hex), and rejection of missing/wrong/promptId-mismatched/
  exit-code-mismatched HMACs. See
  [CHANGELOG.md §0.6.31](CHANGELOG.md#0631--2026-04-17).

### 🔌 Plugins — trigger system

- ✅ **Trigger rules** with `instant` flag and the full iTerm2 action
  set — `bell`, `inject`, `run_script`, `notify`, `sound`, `command`
  shipped in 0.6.9; the three deferred grid-mutation actions
  `highlight_line`, `highlight_text`, and `make_hyperlink` shipped in
  0.6.13 via a new `TerminalGrid` line-completion callback so matches
  map to exact col ranges on a real row before the row scrolls into
  scrollback. Full parity with the iTerm2 trigger doc. See
  [CHANGELOG.md §0.6.9](CHANGELOG.md#069--2026-04-14) and
  [CHANGELOG.md §0.6.13](CHANGELOG.md#0613--2026-04-14).
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

- ✅ **SIMD VT-parser scan**. Shipped in 0.6.23. Ground-state hot path
  now scans 16 bytes at a time via SSE2 (x86_64) / NEON (ARM64) for
  the next non-printable-ASCII byte, then bulk-emits `Print` actions
  for the safe run without running the full state machine. A signed-
  compare trick (XOR 0x80 → two `cmpgt_epi8` against pre-computed
  bounds) flags any interesting byte with one `movemask`. Regression
  guard: `tests/features/vtparser_simd_scan/` asserts byte-identical
  action streams across whole-buffer, byte-by-byte, and pseudo-random-
  chunk feeds over a 38-case corpus covering every offset-mod-16
  alignment for the scan boundary. See
  [CHANGELOG.md §0.6.23](CHANGELOG.md#0623--2026-04-15).
- ✅ **Decouple read/parse/render thread**. Shipped in 0.6.34. PTY read
  and VT parse now run on a dedicated worker QThread (`VtStream`);
  parsed `VtAction` batches are shipped to the GUI over a
  `Qt::QueuedConnection` and applied to `TerminalGrid` there, so paint
  stays on the main thread. Back-pressure: at most 8 batches in flight
  (≈128 KB of unprocessed PTY bytes) before the worker disables its
  `QSocketNotifier` and lets the kernel apply flow control to the
  child; GUI re-enables on drain. PTY writes (keystrokes, DA/CPR/OSC 52
  responses) cross back to the worker via
  `QMetaObject::invokeMethod(... Qt::QueuedConnection)`. Resize goes
  through `Qt::BlockingQueuedConnection` so the PTY winsize is updated
  before the next paint. The `ANTS_SINGLE_THREADED=1` kill-switch was
  retired in 0.6.37 once the new path proved out in the wild (0.6.34
  → 0.6.37 bake period) and the legacy single-threaded code paths were
  deleted. Four feature tests lock the invariants: parse equivalence
  across 11 fixtures × 6 chunking strategies, response ordering, the
  source-level resize-synchronicity contract, and the ptyWrite-gating
  regression pin. See
  [CHANGELOG.md §0.6.34](CHANGELOG.md#0634--2026-04-18) and
  [§0.6.37](CHANGELOG.md#0637--2026-04-18).
- ✅ **Incremental reflow on resize**. Shipped in 0.6.15. The original
  research note called for tracking `wrap_col` per line; the actual
  implementation works on equivalent information already on the line
  (`softWrapped` flag + a single-pass content-width check): standalone
  lines that fit the new width get an in-place `cells.resize()` with
  default-attr padding or trailing-blank trim, skipping the
  allocation-heavy `joinLogical` / `rewrap` round-trip. Multi-line
  soft-wrap sequences still go through the full logic so correctness
  is preserved. See
  [CHANGELOG.md §0.6.15](CHANGELOG.md#0615--2026-04-14).

### 🖥 Platform

- 📋 **Wayland-native Quake-mode**: implement a layer-shell-based
  dropdown via `wlr-layer-shell-unstable-v1`; global hotkey via
  KGlobalAccel on KDE, dbus on GNOME. XCB path stays for X11.

### 🔒 Security

- ✅ **Plugin capability audit UI**. Shipped in 0.6.11. Settings →
  **Plugins** lists every discovered plugin and renders each declared
  permission as a checkbox — checked = granted, unchecked = revoked.
  Revocations persist to `config.plugin_grants[<name>]` and take
  effect at next plugin reload (matches the first-load permission
  prompt's grant semantics). See
  [CHANGELOG.md §0.6.11](CHANGELOG.md#0611--2026-04-14).
- ✅ **Image-bomb defenses**. Shipped in 0.6.11. New
  `TerminalGrid::ImageBudget` tracks total decoded image bytes
  across the inline-display vector + the Kitty cache; cap is **256 MB
  per terminal**. Sixel rejects up front from declared raster size;
  Kitty PNG / iTerm2 OSC 1337 reject post-decode. Inline red error
  text surfaces the rejection (no desktop notification). Per-image
  dimension cap (`MAX_IMAGE_DIM = 4096`) was already in place and
  remains stricter than the 16384 the original ROADMAP item called
  for. See
  [CHANGELOG.md §0.6.11](CHANGELOG.md#0611--2026-04-14).

### 📦 Distribution readiness (H1–H4)

Near-term packaging-polish work — all fully additive, zero C++ code
changes, no new runtime dependencies. Together these four bundles
take Ants from "side project on GitHub" to "package a distro
maintainer could pick up tomorrow." See the
[distribution-adoption overview](#distribution-adoption-overview)
for the full multi-release plan.

- ✅ **H1 — `SECURITY.md` + `CODE_OF_CONDUCT.md`**. Shipped in 0.6.16.
  Coordinated-disclosure policy with supported-versions table,
  reporting channel (GitHub Security Advisory + encrypted email),
  disclosure timeline, severity rubric, in/out of scope lists, and an
  acknowledgement of the hardening we already do. Contributor Covenant
  2.1 verbatim with a dedicated maintainer email + the private GitHub
  Security Advisory listed as conduct reporting channels. Clears the
  Debian / Fedora / Ubuntu security-team review gate. See
  [CHANGELOG.md §0.6.16](CHANGELOG.md#0616--2026-04-14).
- ✅ **H2 — AppStream metainfo + polished desktop entry**. Shipped in
  0.6.17. `packaging/linux/org.ants.Terminal.metainfo.xml` (AppStream
  1.0 with summary / description / releases / categories / keywords /
  OARS content rating / supports / provides / launchable) and
  `packaging/linux/org.ants.Terminal.desktop` (reverse-DNS id,
  tightened Keywords, StartupWMClass, two Desktop Actions for
  NewWindow + QuakeMode). CMake install rules via `GNUInstallDirs`
  cover desktop / metainfo / six hicolor icons, and CI runs
  `appstreamcli validate --explain` + `desktop-file-validate` on every
  push. `MimeType=` entries deliberately omitted — Ants doesn't
  register any URL-scheme handlers, so claiming them would be false
  metadata. Follow-up: add real UI screenshots under
  `docs/screenshots/` and a `<screenshots>` block in the metainfo so
  GNOME Software tiles render a preview instead of the app icon. See
  [CHANGELOG.md §0.6.17](CHANGELOG.md#0617--2026-04-14).
- ✅ **H3 — Man page**. Shipped in 0.6.18.
  `packaging/linux/ants-terminal.1` in `groff -man` covering synopsis,
  description, every CLI flag (`-h`/`--help`, `-v`/`--version`,
  `--quake`/`--dropdown`, `--new-plugin <name>` with the full
  validation contract), environment variables (`SHELL`, `HOME`,
  `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `ANTS_PLUGIN_DEV`,
  `QT_QPA_PLATFORM`), files
  (`~/.config/ants-terminal/{config.json,themes,plugins}`,
  `~/.local/share/ants-terminal/{sessions,recordings,logs}`,
  `<project>/{audit_rules.json,.audit_suppress}`), exit status (the
  four `--new-plugin` codes), bugs (GitHub issues link, SECURITY.md
  for embargoed reports), authors, and see-also (xterm, konsole,
  gnome-terminal, tmux, ssh, forkpty(3), appstreamcli,
  desktop-file-validate). CMake install rule to
  `${CMAKE_INSTALL_MANDIR}/man1/`. CI lints the source with
  `groff -man -Tutf8 -wall …` so syntax regressions fail the build
  the same way `appstreamcli` / `desktop-file-validate` do for H2.
  See [CHANGELOG.md §0.6.18](CHANGELOG.md#0618--2026-04-15).
- ✅ **H4 — Shell completions (bash / zsh / fish)**. Shipped in
  0.6.19. `packaging/completions/ants-terminal.bash` registers an
  `_ants_terminal` function via `complete -F`;
  `packaging/completions/_ants-terminal` is a `#compdef` script using
  `_arguments` with proper exclusion groups (`--quake` ⇄ `--dropdown`,
  `-h` / `-v` short-circuit); `packaging/completions/ants-terminal.fish`
  uses one `complete -c` declaration per flag with manpage-aligned
  descriptions. CMake installs each to the conventional vendor
  location (`${datarootdir}/bash-completion/completions/ants-terminal`,
  `${datarootdir}/zsh/site-functions/_ants-terminal`,
  `${datarootdir}/fish/vendor_completions.d/ants-terminal.fish`); all
  three are auto-discovered on system-wide installs. CI lints each
  file with the matching shell's `-n` / `--no-execute` flag so syntax
  regressions fail the build. Closes the H1–H4 distribution slice;
  remaining packaging work (H5 distro recipes, H6 Flatpak, H7 docs
  site) lives in 0.8.0+. See [CHANGELOG.md §0.6.19](CHANGELOG.md#0619--2026-04-15).

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
- 📋 **Upgrade `actions/checkout` to v5 (Node.js 24).** GitHub
  Actions deprecates Node.js 20 on 2026-06-02 (forced Node 24 by
  default) and removes it entirely on 2026-09-16. Our
  `.github/workflows/ci.yml` pins
  `actions/checkout@11bd71901bbe5b1630ceea73d27597364c9af683  # v4.2.2`
  at two sites (lines 28 and 120), both of which still use Node 20.
  Bump to the latest `v5.x` SHA (currently
  `actions/checkout@v5`) before the June deadline so CI doesn't start
  emitting deprecation warnings or, later, fail outright. Low-risk
  bump — checkout's input surface is stable across v4→v5; the only
  breaking change is the Node runtime. If we add other GitHub-
  provided actions (setup-qt, cache, upload-artifact), audit each
  for Node 20 at the same time.

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

### 📦 Distribution readiness (H5–H7, H13)

Mid-term packaging work — this is where Ants moves from
"packageable-in-theory" to "actually installable on every major
distro." Each sub-bullet can ship independently once H1–H4 land.

- ✅ **H5 — ready-to-submit distro packaging files**. Shipped in
  0.6.20. Each recipe drives the existing CMake install rules with
  zero source patches and runs the audit-rule regression suite in
  its own test stage:
  - `packaging/opensuse/ants-terminal.spec` — openSUSE RPM spec
    targeting Tumbleweed. Uses core macros (`%cmake`,
    `%cmake_build`, `%cmake_install`, `%ctest`, `%autosetup`) so
    the file is close to portable to Fedora. BuildRequires
    declared via `cmake(Qt6*)` pkgconfig-style entries; `%files`
    enumerates all fifteen install paths explicitly so a missing
    artefact fails the OBS build instead of producing a
    silently-incomplete package. Submit via OBS to
    `devel:languages:misc` or a dedicated project; changelog lives
    in a separate `.changes` file per openSUSE convention.
  - `packaging/archlinux/PKGBUILD` — Arch AUR release recipe
    (`ants-terminal`). `check()` runs ctest; built against system
    Qt6 and lua. `sha256sums=('SKIP')` in-tree with a comment
    pointing packagers at `updpkgsums` once the upstream tag is
    live. The `-git` rolling variant is documented in
    `packaging/README.md` (three-line diff: `pkgname`, `source`,
    `pkgver()`).
  - `packaging/debian/` — `control`, `rules`, `changelog`,
    `copyright`, `source/format`. `debhelper-compat 13` drives
    `dh $@ --buildsystem=cmake` with Ninja as the backend; DEP-5
    `copyright` carries the full MIT license text;
    `DEB_BUILD_MAINT_OPTIONS = hardening=+all` stacks
    dpkg-buildflags' hardening wrappers on top of our CMake
    hardening flags. Suitable for `debuild -uc -us` or a Launchpad
    PPA; eventual target is ITP → official archive.
  - `packaging/README.md` — one-page build / submission guide for
    all three recipes.

  See [CHANGELOG.md §0.6.20](CHANGELOG.md#0620--2026-04-15).
- 📋 **H6 — Flatpak packaging**. Ship `org.ants.Terminal.yml`
  against `org.kde.Platform//6.7`. PTY inside Flatpak needs
  `flatpak-spawn --host` — Ghostty precedent. Submit to Flathub
  once stable. One artifact that runs on every distro unlocks
  sandboxed adoption without per-distro packaging work.
- 📋 **H7 — project website + docs site**. Static GitHub Pages site
  at `ants-terminal.github.io` (or equivalent) with: screenshots,
  installation instructions (once H5/H6 land), plugin authoring
  guide (move `PLUGINS.md` body here, keep the file as a pointer),
  quickstart, architecture overview, video/asciicast demos.
  Content-as-code (markdown → static site generator) so the docs
  ship from the same repo.
- 📋 **H13 — distro-outreach launch**. Once H1–H7 are shipped:
  file **intent-to-package** bugs / RFPs in Debian / Fedora /
  NixOS / openSUSE / Arch (as applicable); write a
  **"Why Ants Terminal"** post for r/linux, Hacker News, Phoronix
  tip line, LWN. Focus angle: **the only Linux terminal with a
  built-in capability-audited Lua plugin system + AI triage +
  first-class shell-integration blocks**. Measure via watching
  the GitHub stars + install metrics, not vanity.

### 🖥 Platform

(See the 📦 Distribution readiness section above for the Flatpak +
source-package packaging work; items that don't fit there live
here.)

---

## 0.9.0 — platform + a11y (target: 2026-10)

**Theme:** reach new users. Port, accessibility, internationalization.

### 🖥 Platform

- 📋 **H8 — macOS port**. Qt6 ports cleanly; replace `forkpty` with
  `posix_spawn` + `openpty`, swap `xcbpositiontracker` for
  `NSWindow` KVO observers, sign+notarize the `.app`. Expands the
  addressable audience — a terminal that only runs on Linux is not
  a "Linux terminal project", it's a "Linux-only terminal" —
  distinction matters for cross-platform press coverage.
- 💭 **H12 — Windows port**. ConPTY via `CreatePseudoConsole`
  replaces PTY; `xcbpositiontracker` becomes a no-op. Qt6's
  Windows platform plugin handles the rest. Sign + ship MSI /
  MSIX. Moved to Beyond 1.0 in practice — gating on macOS port
  completing first.

### 🖥 Accessibility

- 📋 **H9 — AT-SPI/ATK support**. Qt6 has AT-SPI over D-Bus natively.
  Work: implement `QAccessibleInterface` for `TerminalWidget`
  exposing role `Terminal`; fire `text-changed` / `text-inserted`
  on grid mutations (gate on OSC 133 `D` markers to batch); expose
  cursor as caret
  ([freedesktop AT-SPI2](https://www.freedesktop.org/wiki/Accessibility/AT-SPI2/)).
  Without this, Orca reads nothing in the terminal. Ubuntu /
  Fedora accessibility review gates on this.
- 💭 **Screen-magnifier-friendly rendering**: honor
  `QGuiApplication::styleHints()->mousePressAndHoldInterval()` and
  provide high-contrast theme variants.

### 🌍 Internationalization

- 📋 **H10 — i18n scaffolding**. Qt's `lupdate` / `linguist` flow;
  wrap all UI strings with `tr()`; ship `.qm` files in
  `assets/i18n/`. Today we have zero `tr()` usage. Start with
  English → Spanish, French, German as a proof of concept. Some
  distros gate review on this.
- 💭 **Right-to-left text support** — bidirectional text in the grid.
  Non-trivial; defer until demand is concrete.

### 📦 Distribution readiness (H11)

- 💭 **H11 — reproducible builds + SBOM**. Build under
  `SOURCE_DATE_EPOCH` so binary hashes are deterministic; generate
  an SPDX SBOM (`spdx-tools` or `syft`) alongside release
  artifacts. Reproducibility is a distro / supply-chain trust
  signal; the SBOM gives downstream security teams (Debian,
  NixOS) a machine-readable dep inventory without having to scrape
  our build system.

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
  screenshot + one animated demo. Rolls up into H7 (docs site).
- 📋 **External security audit**. `SECURITY.md` disclosure policy
  itself ships early under H1 (0.7.0); the 1.0 item is the
  **external** audit — budget a third-party review of the VT
  parser, plugin sandbox, and OSC-8/OSC-52 surfaces before
  stamping 1.0.
- 📋 **H14 — bus factor ≥ 2 + governance doc**. Second maintainer
  with commit rights; a short `GOVERNANCE.md` describing
  decision-making, release process, conflict resolution. Distros
  treat single-maintainer projects as a risk — a documented
  second maintainer clears the bar.
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

### 📦 Distribution & community (H15–H16)

- 💭 **H15 — conference presence**. FOSDEM lightning talk or a
  devroom slot (the Linux desktop devroom is where distro
  maintainers converge). Other options: LinuxFest Northwest,
  Everything Open, SCaLE. One talk reaches more maintainers than
  a hundred issues. Submit in the CFP window for whatever
  conference the project is scope-ready for at the time.
- 💭 **H16 — sponsorship / funding model**. GitHub Sponsors + Open
  Collective. Even small recurring funding signals project
  longevity to distro security teams (they care about "who pays
  for the 30-day CVE response?"). Tiered: individual ($5/mo),
  plugin-author ($20/mo with logo on docs site), corporate
  ($250/mo with logo + priority issue triage).

---

## How to propose a roadmap item

Open a GitHub issue with:

1. **What**: one-sentence description of the capability or change.
2. **Why**: concrete user problem it addresses. Link to the source
   (forum post, issue, personal story) if possible.
3. **Prior art**: has another terminal solved this? Which one, how?
   Link the source.
4. **Scope hint**: is this a weekend change, a week, or a month?
5. **Category**: which theme (🎨/⚡/🔌/🖥/🔒/🧰/📦) does it belong to?

Items that fit naturally into the current release arc land in that
release. Items that don't get queued here with a 💭 marker until
the scope clarifies.

**A roadmap item is not a commitment.** It's a record that we thought
about it, found the prior art, and believe it fits the product. Scope
changes with bandwidth and real-world feedback. Check the
[CHANGELOG.md](CHANGELOG.md) for what actually shipped.
