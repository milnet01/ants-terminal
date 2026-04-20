# Flathub submission checklist

Living playbook for submitting (and later refreshing) Ants Terminal on
[Flathub](https://flathub.org). Pairs with H6.2 in `ROADMAP.md`.

## Prerequisites

Before opening the submission PR, all of the following must be true:

1. **Latest release builds locally as a Flatpak**, with Lua plugins
   loading inside the sandbox. From repo root:
   ```bash
   flatpak-builder --install --user --force-clean \
       build-flatpak packaging/flatpak/org.ants.Terminal.yml
   flatpak run org.ants.Terminal
   ```
   Inside the running Flatpak, confirm:
   - Host shell launches (bash/zsh/fish via `flatpak-spawn --host`).
   - `Settings → Plugins` shows the plugin manager UI (plugins enabled).
   - A small test plugin at `~/.config/ants-terminal/plugins/smoketest/init.lua`
     loads without error.

2. **Manifest lint is clean.**
   ```bash
   flatpak run --command=flatpak-builder-lint org.flatpak.Builder \
       manifest packaging/flatpak/org.ants.Terminal.yml
   ```

3. **Metainfo validates online.** (The `raw.githubusercontent.com`
   screenshot URLs must already be live on the `main` branch.)
   ```bash
   appstreamcli validate packaging/linux/org.ants.Terminal.metainfo.xml
   ```

4. **All 32 feature tests pass.**
   ```bash
   cd build && ctest --output-on-failure
   ```

5. **Tag is pushed.** The Flathub manifest pins `tag: v<VERSION>`; the
   tag must exist on GitHub before the PR goes in or CI cannot check
   out the source.
   ```bash
   git tag v0.7.3 && git push origin v0.7.3
   ```

## Opening the Flathub PR

1. **Fork** [`flathub/flathub`](https://github.com/flathub/flathub)
   and check out a `new-pr` branch off `master`.

2. **Generate the Flathub-shaped manifest:**
   ```bash
   packaging/flatpak/make-flathub-manifest.sh \
       > /path/to/flathub-fork/org.ants.Terminal/org.ants.Terminal.yml
   ```
   (Version auto-detected from `CMakeLists.txt`; pass an explicit
   `<version>` argument to override.)

3. **Copy the metainfo + desktop files** into the same subdirectory:
   ```bash
   cp packaging/linux/org.ants.Terminal.metainfo.xml \
      /path/to/flathub-fork/org.ants.Terminal/
   cp packaging/linux/org.ants.Terminal.desktop \
      /path/to/flathub-fork/org.ants.Terminal/
   cp assets/ants-terminal-256.png \
      /path/to/flathub-fork/org.ants.Terminal/org.ants.Terminal.png
   ```

4. **Open the PR** against the `new-pr` branch of `flathub/flathub`
   with subject `Add org.ants.Terminal` and a body describing the app
   in one paragraph. Flathub CI runs a build in the PR; iterate on
   failures until green.

5. **After merge:** Flathub provisions a new repo at
   `flathub/org.ants.Terminal` with the submitted files. Future
   version bumps + Lua tarball refreshes go there, not here —
   `flatpak-external-data-checker` opens those PRs automatically.

## On every subsequent release

For each `v<VERSION>` tag after the initial Flathub landing:

1. Push the tag on this repo.
2. Clone the downstream `flathub/org.ants.Terminal` repo.
3. Regenerate the manifest body:
   ```bash
   packaging/flatpak/make-flathub-manifest.sh \
       > /path/to/flathub-repo/org.ants.Terminal.yml
   ```
4. Re-copy the metainfo XML (release notes for the new version).
5. Open a PR against `master` in the Flathub repo. CI rebuilds; on
   green, merge — Flathub delivers the update to end users.

## Flip the ROADMAP on landing

Once the first `flathub/flathub` PR merges and the build shows up at
`flathub.org/apps/org.ants.Terminal`:

- Flip `H6.2` in `ROADMAP.md` from 📋 to ✅.
- Flip the "gating item 1: no distro packages anywhere" entry in the
  Distribution-adoption overview from "H5 + H6 unblock this" to
  "unblocked".
- Add a `## [<NEXT>]` section to `CHANGELOG.md` announcing the
  Flathub landing (user-facing: one-command install via
  `flatpak install flathub org.ants.Terminal`).

## Regression safety

- `tests/features/flathub_manifest_transform/` pins the transformer
  output shape — a regression in `make-flathub-manifest.sh` fails the
  test at `ctest` time, not at Flathub-PR time.
- `tests/features/flatpak_lua_module/` pins the Lua module shape in
  the dev manifest, which the transformer preserves byte-identical.
- `tests/features/flatpak_host_shell/` pins the `ptyhandler.cpp` host-
  shell detection — a regression that breaks the Flatpak shell would
  fail CI before anything reaches Flathub.
