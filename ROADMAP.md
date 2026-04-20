# Ants Terminal — Roadmap

> **Current version:** 0.7.4 (2026-04-20). See [CHANGELOG.md](CHANGELOG.md)
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
| **H6** | Flatpak manifest (`org.ants.Terminal.yml`) + host-shell wiring | ✅ | 0.7.2 |
| **H6.1** | Lua 5.4 module in Flatpak manifest so plugins work inside the sandbox | ✅ | 0.7.3 |
| **H6.2** | Flathub submission — PR `org.ants.Terminal` against `flathub/flathub` | 📋 | 0.8.0 |
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
4. **Linux-only** + ~~**X11-only dropdown**~~ — H8 (macOS) widens the addressable audience; Wayland-native Quake positioning + global-hotkey registration both shipped (0.6.38 + 0.6.39).

---

## Per-store publication playbook

The H-bundle table above tracks **preparation** work (the files that
need to exist in-tree). This section tracks **submission** work —
getting the prepared package actually **live** in each store's
listing. Each store has its own review cadence, ingestion format,
and maintainer expectations, so they're broken out as discrete items
with step-by-step playbooks.

Stores are ordered by **ease of publish × reach**: Flathub + AUR
first (high reach, low friction), official distro archives last
(highest friction, longest lead time). A store is considered
"shipped" when `ants-terminal` installs from the stock package
manager without extra repository configuration.

| Store | Format | Prep status | Publish status | Target release |
|-------|--------|-------------|----------------|----------------|
| **P1 — Flathub** | Flatpak | ✅ manifest + Lua module (H6, H6.1) | 📋 submit `org.ants.Terminal` to `flathub/flathub` (H6.2) | 0.8.0 |
| **P2 — AUR (Arch User Repository)** | PKGBUILD | ✅ recipe (H5) | 📋 push to `aur.archlinux.org/packages/ants-terminal.git` + `-git` variant | 0.8.0 |
| **P3 — openSUSE OBS (home:ants-terminal)** | RPM spec | ✅ spec (H5) | 📋 upload to Open Build Service `home:` project, then target Tumbleweed / Leap repos | 0.8.0 |
| **P4 — Fedora COPR** | RPM spec | ✅ spec (H5) | 📋 create `ants-terminal` COPR; later submit package review for `fedora-extras` | 0.8.0 |
| **P5 — Debian / Ubuntu PPA** | deb source | ✅ debian/ tree (H5) | 📋 upload signed source package to Launchpad PPA | 0.8.0 |
| **P6 — Snap Store (snapcraft.io)** | snapcraft.yaml | 📋 write `snap/snapcraft.yaml`, host-shell model like Flatpak | 📋 register `ants-terminal` snap name + publish via `snapcraft upload` | 0.9.0 |
| **P7 — AppImage** | AppImage | 📋 write `ants-terminal.AppImage.yml` (linuxdeploy / appimage-builder) | 📋 publish nightly + release artifacts on GitHub Releases | 0.9.0 |
| **P8 — Nixpkgs (NixOS)** | nix expression | 📋 write `pkgs/applications/terminal-emulators/ants-terminal/default.nix` | 📋 open PR against `NixOS/nixpkgs` master | 0.9.0 |
| **P9 — Debian (main archive)** | deb, official | 📋 RFP → ITP → sponsored upload → new-queue review | 📋 file ITP bug, find DM sponsor | 1.0.0+ |
| **P10 — Ubuntu (universe, via Debian)** | deb | Auto-sync from Debian unstable | 📋 rides on P9 | 1.0.0+ |
| **P11 — Fedora (official, via Bodhi)** | RPM | 📋 package review bug → sponsor → Bodhi updates | 📋 rides on P4 quality signals | 1.0.0+ |
| **P12 — Arch extra** | PKGBUILD, trusted | Would need TU sponsorship and high AUR install count | 💭 not a goal until reach justifies it | 1.0.0+ |

### P1 — Flathub submission

**Prerequisites:** H6 ✅, H6.1 ✅, H6.2 in-tree prep ✅ (screenshots,
`make-flathub-manifest.sh`, FLATHUB.md playbook).
**Blocker:** real-user shakedown of the 0.7.3 Flatpak before claiming
the Flathub repo name.

1. **Tag the release.** `git tag -s v<version>; git push --tags`.
2. **Regenerate the tag-pinned manifest:**
   `packaging/flatpak/make-flathub-manifest.sh v<version> >
   /tmp/org.ants.Terminal.yml`.
3. **Fork [flathub/flathub](https://github.com/flathub/flathub).**
   In the fork, create a **new repo**
   `flathub/org.ants.Terminal` per the Flathub "Submit via a new
   repo" flow
   (https://docs.flathub.org/docs/for-app-authors/submission/).
4. **Populate the repo** with the generated manifest, a copy of
   `org.ants.Terminal.metainfo.xml` (from `packaging/linux/`), and
   a copy of `org.ants.Terminal.desktop`. Pin `branch: stable` in the
   manifest so Flathub CI builds against our stable branch.
5. **Open a PR** from `flathub/org.ants.Terminal` against
   `flathub/flathub` with the repo URL in the PR body. Flathub CI
   rebuilds on each push.
6. **Respond to review comments** (typical first-review: shrink
   `finish-args` surface, confirm the `flatpak-external-data-checker`
   stanza updates without manual intervention).
7. **On merge,** Flathub publishes automatically. `flatpak install
   flathub org.ants.Terminal` is the smoke test.
8. **Post-merge maintenance:** each new `v<version>` tag requires a
   manifest PR against `flathub/org.ants.Terminal` pointing to the
   new tag (single script invocation — `make-flathub-manifest.sh
   v<new>` → commit → PR). Flathub CI handles the Lua-tarball hash
   refresh on its own via `x-checker-data`.
9. **Flip the gating-item entry:** "No distro packages anywhere" →
   "unblocked via Flathub."

### P2 — AUR publish

**Prerequisites:** H5 ✅ (`packaging/archlinux/PKGBUILD` in tree).
**Blocker:** need an `aur.archlinux.org` maintainer account and an
SSH key registered there.

1. **Create the `ants-terminal` AUR page** via `git clone
   ssh://aur@aur.archlinux.org/ants-terminal.git` (empty repo created
   on first push).
2. **Copy `packaging/archlinux/PKGBUILD`** into the checkout, along
   with a generated `.SRCINFO` (`makepkg --printsrcinfo > .SRCINFO`).
3. **Refresh the tarball hash:** `updpkgsums` resolves the
   `sha256sums=('SKIP')` placeholder against the GitHub tag.
4. **Push** — the AUR git hook publishes immediately. No review
   queue.
5. **Repeat for the `-git` rolling variant:** second repo
   `ants-terminal-git` with the three-line diff documented in
   `packaging/README.md` (`pkgname`, `source`, `pkgver()`).
6. **Maintenance per release:** `updpkgsums && makepkg --printsrcinfo
   > .SRCINFO && git commit && git push`. Script candidate:
   `scripts/publish-aur.sh`.

### P3 — openSUSE Build Service (OBS)

**Prerequisites:** H5 ✅ (`packaging/opensuse/ants-terminal.spec`).
**Blocker:** `openSUSE Build Service` account
(`build.opensuse.org`).

1. **Log in to OBS** and create a `home:<username>:ants-terminal`
   project.
2. **`osc mkpac ants-terminal`** inside the project; drop in
   `ants-terminal.spec`, the source tarball reference, and a
   `.changes` file mirroring CHANGELOG.md in OBS's expected format.
3. **`osc build`** locally against Tumbleweed to catch build
   errors before upload. Then `osc commit`.
4. **OBS auto-builds** against Tumbleweed, Leap 15.x, and any other
   repos added to the project. Results visible at
   `build.opensuse.org/package/show/home:<username>:ants-terminal/ants-terminal`.
5. **Publish to stable repositories:** after green builds, submit a
   `submitrequest` to `devel:languages:misc` (or a dedicated devel
   project). Once accepted, package enters the distro review queue
   for inclusion in the next Tumbleweed / Leap repos.
6. **One-click-install URL:** OBS auto-generates
   `software.opensuse.org/package/ants-terminal` pages for discovery.

### P4 — Fedora COPR

**Prerequisites:** H5 ✅ (spec is Fedora-compatible — uses `%cmake`
macros that work on both Fedora and openSUSE).
**Blocker:** Fedora Account System (FAS) account.

1. **Create a COPR project** at `copr.fedorainfracloud.org/coprs/`
   with Fedora 40/41/42/rawhide chroots enabled.
2. **Upload the spec** via `copr-cli build <project>
   /path/to/ants-terminal.spec` (or sync the GitHub repo with COPR's
   `custom build` webhook for auto-rebuild on tag push).
3. **Smoke-test:** `sudo dnf copr enable <username>/ants-terminal &&
   sudo dnf install ants-terminal`.
4. **Later — submit a package review** against the `Package Reviews`
   Bugzilla product for inclusion in `fedora-extras`. This is the
   start of the Fedora official-archive path (P11).

### P5 — Debian / Ubuntu PPA (Launchpad)

**Prerequisites:** H5 ✅ (`packaging/debian/` tree).
**Blocker:** Launchpad account + a PGP signing key registered there.

1. **Create a PPA** at `launchpad.net/~<user>/+archive/ubuntu/ants-terminal`.
2. **Build a signed source package locally:** `debuild -S -sa` from
   the project root (the debian/ tree is at `packaging/debian/`, so
   symlink or copy it to `./debian/` first).
3. **Upload:** `dput ppa:<user>/ants-terminal
   ../ants-terminal_<version>-1_source.changes`. Launchpad rebuilds
   for each supported Ubuntu series (jammy / noble / oracular).
4. **Smoke-test:** `sudo add-apt-repository ppa:<user>/ants-terminal
   && sudo apt update && sudo apt install ants-terminal`.
5. **Backports note:** Debian users can install the same `.deb`
   directly — Ubuntu PPAs work as a generic Debian-family archive
   when users add the sources.list line manually.

### P6 — Snap Store

**Prerequisites:** write `snap/snapcraft.yaml`, which does NOT
exist yet. Host-shell model similar to our Flatpak approach.

1. **Write `snap/snapcraft.yaml`** targeting `core24`, `confinement:
   strict`, with `plugs:` for `home`, `network`, `wayland`,
   `desktop`, `desktop-legacy`, `opengl`, `pulseaudio`,
   `gsettings`. PTY path mirrors the Flatpak host-shell wiring
   (see `ptyhandler.cpp`'s `FLATPAK_ID` branch — needs a parallel
   `SNAP` branch).
2. **Test locally:** `snapcraft pack` + `sudo snap install
   --dangerous ants-terminal_<version>_amd64.snap`.
3. **Register the name:** `snapcraft register ants-terminal`.
4. **Upload:** `snapcraft upload ants-terminal_<version>_amd64.snap
   --release=stable`. Review by Snap Store is typically hours to
   days on first submission (manual review for `strict`
   confinement + `home` plug), then auto-approved for revisions.
5. **Ongoing:** `snapcraft.io` GitHub builder hooks auto-rebuild
   on tag push — no per-release manual work after the initial
   submission.

### P7 — AppImage

**Prerequisites:** write an AppImage recipe. Not yet started.

1. **Choose tool:** `linuxdeploy` with its Qt plugin is the standard
   for Qt-based AppImages (handles Qt6, QML plugins, platform
   integrations).
2. **Add a CI lane** (`.github/workflows/appimage.yml`) that runs on
   tag push: `make` the project, `linuxdeploy --appdir AppDir
   --plugin qt --output appimage`, attach the resulting
   `Ants_Terminal-<version>-x86_64.AppImage` to the GitHub Release.
3. **(Optional) publish to AppImageHub** (`appimage.github.io`)
   by opening a PR with a YAML metadata stub. AppImageHub is a
   listing directory, not a store — it points at our GitHub
   Release artifact.

### P8 — Nixpkgs

**Prerequisites:** write the nix expression. Not yet started.

1. **Fork `NixOS/nixpkgs`.**
2. **Add `pkgs/applications/terminal-emulators/ants-terminal/default.nix`**
   with `mkDerivation` using `cmake`, `qt6.qtbase`,
   `qt6.qtwayland`, `qt6.qtopengl`, `lua5_4`, etc. Pin
   `src = fetchFromGitHub { … rev = "v<version>"; hash = "sha256-…"; }`.
3. **Wire it in `pkgs/top-level/all-packages.nix`.**
4. **Smoke-test:** `nix-build -A ants-terminal` in the fork.
5. **Open PR** against `NixOS/nixpkgs`. First-submission reviews
   are thorough (style, dependency minimization, `meta` fields);
   budget 1–4 weeks of review cycles.
6. **On merge,** `nix profile install nixpkgs#ants-terminal` works
   for every NixOS + non-NixOS Nix user.

### P9 — Debian (official archive)

**Prerequisites:** P5 working, plus a Debian Developer (DD) sponsor.
Longest lead time of any store.

1. **File an RFP** (Request For Packaging) bug against
   `wnpp` pseudo-package in Debian BTS.
2. **Work with a sponsor** (DD or DM) to iteratively polish the
   packaging (lintian clean, copyright audit, build on buildd
   network). Our `packaging/debian/` tree is a starting point —
   expect several rounds of refinement.
3. **ITP** (Intent To Package): once polished, the sponsor files
   an ITP bug and uploads the source package via their credentials.
4. **New-queue review:** ftpmaster manually reviews the initial
   upload for license + standards compliance. First-time uploads
   can sit in new-queue for weeks.
5. **After acceptance:** subsequent versions go through normal
   sponsor-upload flow (still requires DD involvement) until I apply
   for DM status (2-year typical timeline).

### P10 — Ubuntu (via Debian)

Rides on P9. Debian unstable auto-syncs to the next Ubuntu dev
release; Ubuntu-specific patches generally aren't needed for a
terminal emulator.

### P11 — Fedora (official archive)

Rides on P4 track record. Submit package review against
`fedora-extras`; once approved, package lands in Fedora rawhide and
follows normal Bodhi update flow into stable releases.

### P12 — Arch extra

Requires Arch Trusted User (TU) sponsorship. Not a near-term goal;
AUR (P2) is the standard Arch path and serves the Arch community
adequately.

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

- ✅ **Wayland-native Quake-mode** — shipped across two releases.
  Part 1 of 2 (0.6.38): `find_package(LayerShellQt CONFIG QUIET)` wires
  `LayerShellQt::Interface` when the `layer-shell-qt6-devel` package
  is installed, and `MainWindow::setupQuakeMode()` promotes the window
  to a `zwlr_layer_surface_v1` at `LayerTop`, anchored top/left/right
  with exclusive-zone 0. The dead `quake_hotkey` config key is wired
  to a `QShortcut` with `Qt::ApplicationShortcut` context. XCB path
  preserved for X11. Source-level invariants pinned in
  `tests/features/wayland_quake_mode/`.
  Part 2 of 2 (0.6.39): `GlobalShortcutsPortal` client wraps the
  Freedesktop `org.freedesktop.portal.GlobalShortcuts` handshake
  (CreateSession → session handle → BindShortcuts → Activated) behind
  a single Qt signal. `MainWindow` binds the `toggle-quake` id when
  the portal service is registered on the session bus (KDE Plasma 6,
  xdg-desktop-portal-hyprland, -wlr); on GNOME Shell the portal call
  fails and the in-app `QShortcut` fallback stays as the activation
  path. Both paths debounce via a 500 ms window to avoid focused
  double-fire. Replaces the originally-planned KGlobalAccel + GNOME
  D-Bus branching with one compositor-agnostic API. See
  [CHANGELOG.md §0.6.38](CHANGELOG.md#0638--2026-04-18) and
  [§0.6.39](CHANGELOG.md#0639--2026-04-18).

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
- ✅ **Upgrade `actions/checkout` to v5 (Node.js 24).** Shipped in
  0.7.0. Both `.github/workflows/ci.yml` pin sites bumped to
  `actions/checkout@93cb6efe18208431cddfb8368fd83d5badbf9bfd  # v5.0.1`
  (v5.0.0 and v5.0.1 both resolve to the same SHA; v5.0.1 is the
  currently-latest patch). Landed in commit `aad4f32`. Input surface
  is stable across v4→v5 — no workflow changes required. See
  [CHANGELOG.md §0.7.0](CHANGELOG.md#070--2026-04-19).

---

## 0.8.0 — multiplexing + marketplace (target: 2026-08)

**Theme:** big new capabilities. This is the "features you'd expect from
a modern terminal" release.

### 🐛 Carried over from 0.7.x

- 💭 **Menubar dropdown flicker on mouse movement.** Partially reduced
  by 0.7.5 (`NoAnimStyle` killed Fusion's 60 Hz `QPropertyAnimation`
  cycle) and the 0.7.5+1 follow-up commit `04f3409` (extended
  intra-action suppression to `HoverMove`/`HoverEnter`/`HoverLeave`).
  Residual flicker still visible to the user on mouse motion over an
  open dropdown. Measured state on 2026-04-20: idle-log event volume
  is clean, but a ~85 Hz `UpdateRequest` → paint cascade persists
  even with `paintEvent()` forced to no-op (`ANTS_SKIP_PAINT=1`) and
  `WA_TranslucentBackground` disabled (`ANTS_OPAQUE_WINDOW=1`),
  which points at KWin's compositor sync handshake driving the
  cycle rather than anything in our widget tree. Likely next-angle
  fixes: (a) request KWin skip the blur-behind effect for our popup
  QMenus (empty `_KDE_NET_WM_BLUR_BEHIND_REGION` property);
  (b) disable `Qt::WA_Hover` on `QMenuBar` while a dropdown is open
  so the style engine stops re-evaluating `:hover` on every cursor
  tick; (c) test under GNOME/Mutter and i3 to confirm KWin is the
  amplifier. Not a blocker (user-reported as "not the biggest issue")
  but tracked so the 0.7.x fix attempts don't get re-invented.

### 🎨 Features — multiplexing

- 📋 **Headless mux server with codec RPC**. WezTerm's architecture
  ([DeepWiki](https://deepwiki.com/wezterm/wezterm/2.2-multiplexer-architecture)):
  `ants-terminal --server` runs without a GUI and accepts attachments
  over a Unix socket; `ants-terminal --attach <socket>` reconnects.
  Panes survive window close. Sparse scrollback fetched on demand via
  `GetLines` RPC.
- 🚧 **Remote-control protocol** (Kitty-style,
  [docs](https://sw.kovidgoyal.net/kitty/rc_protocol/)): JSON envelopes
  over a Unix socket. Commands: `launch`, `send-text`, `set-title`,
  `select-window`, `get-text`, `ls`, `new-tab`. Auth via X25519 when
  a password is set. Unlocks scripting, IDE integration, CI.
  - ✅ **First slice: socket + envelope + `ls`.** `src/remotecontrol.{h,cpp}`
    brings up a `QLocalServer` on `$ANTS_REMOTE_SOCKET` (or the XDG
    runtime default `$XDG_RUNTIME_DIR/ants-terminal.sock`). Each
    connection is one-shot: read a single JSON line, dispatch, write
    the JSON response line, close. `ls` returns
    `{"ok": true, "tabs": [{"index", "title", "cwd", "active"}, ...]}`.
    Unknown commands return `{"ok": false, "error": "unknown command: ..."}`
    with exit code 2. The same binary handles client mode via
    `--remote <cmd>` (optionally `--remote-socket <path>`) — no
    separate client binary. Pinned by source-grep feature test
    `tests/features/remote_control_ls/` (8 invariants including
    field-name stability, env-var override, `--remote` ordering).
  - ✅ **`send-text` command.** Writes a UTF-8 string byte-for-byte to
    a tab's PTY master. `tab` field optional (active tab default),
    `text` required; response carries `bytes` written. Client CLI:
    `--remote-text <str>` or stdin pipe; `--remote-tab <i>` optional.
    Does not auto-append a newline (matches Kitty; callers include
    terminators explicitly). Pinned by
    `tests/features/remote_control_send_text/`.
  - ✅ **`new-tab` command.** Opens a fresh tab and returns its
    0-based index. `cwd` optional (inherits focused-terminal cwd);
    `command` optional (written via `writeCommand` after a 200 ms
    settle). Response: `{"ok":true,"index":<int>}`. Client CLI:
    `--remote-cwd <path>`, `--remote-command <str>`. Pinned by
    `tests/features/remote_control_new_tab/`.
  - 📋 **Next commands** (one per commit, order TBD by user priority):
    `set-title`, `select-window`, `get-text` (read visible screen),
    `launch` (spawn a command in a new tab — convenience wrapper
    over `new-tab` + `send-text`).
  - 💭 **Auth layer.** X25519 shared-secret when `$ANTS_REMOTE_PASSWORD`
    is set. Shipped after the command surface is complete.
- ✅ **SSH ControlMaster** auto-integration from the SSH bookmark
  dialog. Shipped in 0.7.1. Connects opened from the SSH Manager
  carry `-o ControlMaster=auto`,
  `-o ControlPath=$HOME/.ssh/cm-%r@%h:%p`, and
  `-o ControlPersist=10m` when the new `ssh_control_master` config
  key is true (default). `$HOME` resolves in-process via
  `QDir::homePath()` so the ControlPath works under dash / POSIX
  `sh`; `%r@%h:%p` are OpenSSH tokens and survive shell quoting
  intact. Second tab to the same host opens in ms instead of
  seconds. See
  [CHANGELOG.md §0.7.1](CHANGELOG.md#071--2026-04-19).
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

- 🚧 **Terminal throughput slowdowns** (user report 2026-04-20).
  Intermittent stalls observed during normal use. Investigation items:
  (a) profile `onVtBatch()` under heavy-output workloads — `yes`,
  `dd`, `find /`, build logs — and identify hotspots;
  (b) audit `TerminalGrid::processAction` for O(n) allocations on
  each Print (the hot path);
  (c) check whether the async batch drain ack (`drainAck`) under
  back-pressure is creating producer-consumer sawtooth rather than
  steady flow; (d) measure paint time at 2000+ line scrollback with
  ligatures on vs off; (e) check the focus-redirect lambda and other
  `QApplication::focusChanged` handlers for expensive work per event.
  - ✅ **Benchmark harness.** `tests/perf/bench_vt_throughput.cpp`
    drives four fixed corpora — `ascii_print`, `newline_stream`,
    `ansi_sgr`, `utf8_cjk` — through `VtParser` →
    `TerminalGrid::processAction` at release-level `-O2`, no GUI.
    Emits a CSV line per corpus with bytes, actions, wall-ms,
    MB/s, actions/s. Registered under `ctest` label `perf`
    (excluded from the default `fast` suite). Run with
    `ctest -L perf --verbose` or directly; `ANTS_PERF_MB=64
    ./bench_vt_throughput` for a heavier sweep. **Baseline
    2026-04-20 on the dev laptop (4 MB per corpus):**

    | Corpus | MB/s | Actions/s | Note |
    |--------|-----:|----------:|------|
    | `ascii_print` | 23.3 | 24.4 M | Print-only; UTF-8 fast path |
    | `newline_stream` | 5.3 | 5.6 M | **4× slower — scroll/scrollback hotspot** |
    | `ansi_sgr` | 16.9 | 13.0 M | SGR dispatch |
    | `utf8_cjk` | 7.9 | 3.1 M | 3-byte UTF-8 + double-width |

    Top signal from the baseline: `newline_stream` is 4× slower
    than pure Print, pointing at `lineFeed()` →
    `TerminalGrid::scrollUp()` → scrollback deque insertion as
    the dominant cost. Matches sub-item (b) on the list above —
    next action is a `perf record`/`callgrind` run over
    `newline_stream` to confirm which `scrollUp` sub-step
    dominates (row allocation? per-line combining-char table
    copy? pushBack on the scrollback `std::deque`?).
  - ✅ **Main-thread stall detector** (`DebugLog::Perf`, enabled
    via `ANTS_DEBUG=perf`). 200 ms heartbeat `QTimer` that
    records every drift > 100 ms as a main-thread stall. Added
    after the follow-up user report 2026-04-20: "slow down
    experienced at various times, when tab has been clear or
    has had lots of text, not one specific scenario." The
    intermittent-and-content-independent signature points away
    from the PTY hot path and toward a periodic background
    handler (2 s status-bar `updateStatusBar` reading
    `.git/HEAD`, 2 s Claude-integration `/proc` walk, focus-
    redirect lambda, plugin callback, session save, file-system
    watcher fire). The detector fingerprints which one on the
    next reproduction — log line shape is `STALL: main-thread
    blocked for Nms (gap=..., interval=..., count=..., worst=...)`.
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
- ✅ **H6 — Flatpak packaging**. Shipped in 0.7.2.
  `packaging/flatpak/org.ants.Terminal.yml` against
  `org.kde.Platform//6.7` (KDE SDK brings cmake/ninja/Qt6). The PTY
  path in `src/ptyhandler.cpp` detects `FLATPAK_ID` /
  `/.flatpak-info` in the forked child and exec's the user's shell
  via `flatpak-spawn --host` with explicit
  `--env=TERM/COLORTERM/TERM_PROGRAM/TERM_PROGRAM_VERSION/COLORFGBG`
  and `--directory=<workDir>` — the only workable PTY model inside a
  sandbox (same pattern Ghostty's Flathub build uses). `finish-args`
  cover Wayland/X11/DRI, network (AI endpoint + SSH), portals
  (global shortcuts), desktop notifications, and XDG config/data
  directories. Source-grep feature test
  (`tests/features/flatpak_host_shell/`) pins the branch shape:
  detection probes both signals in an OR, `--host` + `--`
  separators, every TERM var passes through as `--env=`, workDir
  gates on `isEmpty()`, direct-exec fallback is preserved verbatim.
  Lua plugins disabled in the initial manifest — `org.kde.Sdk`
  doesn't ship lua54 and tarball-sha256 refresh per release is a
  maintenance cost worth deferring; plugin support returns via a
  `shared-modules` Lua entry in a follow-up (H6.1 below). Flathub
  submission is the final step — manifest is ready to re-point
  `sources[].type: dir` → `git / url / tag` and PR against
  [flathub/flathub](https://github.com/flathub/flathub) (H6.2
  below). See [CHANGELOG.md §0.7.2](CHANGELOG.md#072--2026-04-19)
  and [packaging/flatpak/README.md](packaging/flatpak/README.md).
- ✅ **H6.1 — Lua plugins in Flatpak**. Shipped in 0.7.3. The
  manifest now carries an in-manifest Lua 5.4 `archive` module
  before `ants-terminal`, built from
  `https://www.lua.org/ftp/lua-5.4.7.tar.gz` with a pinned
  `sha256` and the `linux-noreadline` target (Ants only links
  `liblua.a` statically; readline would be bloat in the sandbox).
  `MYCFLAGS="-fPIC"` keeps the library PIE-safe for linking into
  the `ants-terminal` executable; installed to `/app` via
  `make install INSTALL_TOP=/app`, where CMake's `FindLua`
  searches by default. The `x-checker-data` stanza on the module
  is wired to
  [flatpak-external-data-checker](https://github.com/flathub/flatpak-external-data-checker)
  so Flathub CI auto-refreshes the `url` + `sha256` on each Lua
  5.4.x point release — no manual hash churn, and 5.5.x majors
  are excluded (they would break the in-source
  `find_package(Lua 5.4)` floor). `tests/features/flatpak_lua_module/`
  pins six invariants against the manifest YAML (module order,
  pinned sha256, `-fPIC`, install prefix, readline-free target,
  x-checker-data stanza). The `flathub/shared-modules` path
  remains the cleaner long-term home if Flathub ever accepts a
  Lua 5.4 entry — migration would replace the in-manifest module
  with a shared-modules reference; `x-checker-data` would
  continue to fire. See
  [CHANGELOG.md §0.7.3](CHANGELOG.md#073--2026-04-20).
- 📋 **H6.2 — Flathub submission**. PR a new repo against
  [flathub/flathub](https://github.com/flathub/flathub) named
  `org.ants.Terminal`. In-tree prep is complete: the
  `<screenshots>` block in
  `packaging/linux/org.ants.Terminal.metainfo.xml` now points at three
  captures under `docs/screenshots/` so Flathub's store tile renders
  with real UI (main terminal + Review Changes dialog + Project Audit
  panel), and `packaging/flatpak/make-flathub-manifest.sh` + a feature
  test pin the transformation from the dev manifest
  (`type: dir / path: ../..`) to the Flathub manifest
  (`type: git / url: https://github.com/milnet01/ants-terminal /
  tag: v<version>`) so there is one source of truth for the manifest.
  Submission playbook lives at
  [packaging/flatpak/FLATHUB.md](packaging/flatpak/FLATHUB.md).
  Remaining blocker is real-user shakedown of the v0.7.3 Flatpak —
  wait for a small cohort to exercise the host-shell + plugin build
  before claiming the Flathub repo name. Once the PR merges, Flathub
  CI rebuilds on each new `v<version>` tag — no manual work per
  release, only per-bump regeneration of the tag-pinned manifest
  (one script invocation). On landing, flip the "gating item 1: no
  distro packages anywhere" entry in the
  [Distribution-adoption overview](#distribution-adoption-overview)
  from "H5 + H6 unblock this" to "unblocked".
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
