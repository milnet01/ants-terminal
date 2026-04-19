# Flatpak packaging — Ants Terminal

One artifact that runs on every distro. The manifest at
`org.ants.Terminal.yml` is ready to submit to Flathub once the first
tagged release is green through CI.

## Local build

```bash
# Required once:
flatpak install --user flathub org.kde.Platform//6.7 org.kde.Sdk//6.7 \
                               org.flatpak.Builder

# From the project root (manifest is aware of its relative `../..`):
flatpak-builder --install --user --force-clean \
    build-flatpak packaging/flatpak/org.ants.Terminal.yml

flatpak run org.ants.Terminal
```

`--install --user` drops the built app under `~/.local/share/flatpak/app/`.
Reproduce the Flathub CI lint with:

```bash
flatpak run --command=flatpak-builder-lint org.flatpak.Builder \
    manifest packaging/flatpak/org.ants.Terminal.yml
```

## Host shell wiring

Inside the sandbox, `src/ptyhandler.cpp` detects the `FLATPAK_ID`
environment variable (and `/.flatpak-info`) and switches the shell exec
path from `execlp(shell, …)` to
`execvp("flatpak-spawn", ["flatpak-spawn", "--host", "--env=TERM=…", …, "--", shell])`.

The user's shell therefore runs **on the host**, not in the sandbox:

- `$PATH` is the host's. `git`, `cargo`, `make`, `ssh`, etc. all reachable.
- `$HOME` is the host's. Working directories match what the user sees.
- `$SHELL` resolution still uses `getpwuid(getuid())->pw_shell` from
  inside the sandbox — this matches the host login entry because
  `flatpak-spawn --host` starts the command against the host's real
  user record.

The sandbox itself remains confined: the terminal emulator process (the
QOpenGLWidget + VT parser + renderer) cannot touch the host filesystem
except through the declared `--filesystem=` finish-args or portal calls.
Only the child shell escapes. This is the same model Ghostty uses for
its Flathub build.

The wire-through requires `--talk-name=org.freedesktop.Flatpak` in
finish-args (already set) so the sandbox can reach the host-side
`org.freedesktop.Flatpak` helper daemon.

## Lua plugins — limitation

The initial manifest does **not** build Lua 5.4. `org.kde.Sdk//6.7` does
not include `lua54-devel`, and pinning a Lua tarball sha256 in the
manifest requires refreshing the hash each release. Until that's wired
up (either via a `flathub/shared-modules` entry or an in-manifest
`archive` module), the Flatpak build falls back to CMakeLists'
`LUA_FOUND=FALSE` branch — plugins are disabled but the rest of the
terminal is unaffected.

Adding Lua in a follow-up means appending a second module before
`ants-terminal` — rough shape:

```yaml
modules:
  - name: lua
    buildsystem: simple
    sources:
      - type: archive
        url: https://www.lua.org/ftp/lua-5.4.7.tar.gz
        sha256: <refresh via `sha256sum lua-5.4.7.tar.gz`>
    build-commands:
      - make linux MYCFLAGS="-fPIC" -j $FLATPAK_BUILDER_N_JOBS
      - make install INSTALL_TOP=/app

  - name: ants-terminal
    # … existing block
```

See [flatpak-external-data-checker](https://github.com/flathub/flatpak-external-data-checker)
for automatic tarball-hash refresh on Flathub.

## Submitting to Flathub

1. Create a GitHub repo under the [flathub](https://github.com/flathub)
   org named `org.ants.Terminal` (Flathub mints it after the
   new-submission PR at `flathub/flathub` is merged).
2. The repo's `org.ants.Terminal.yml` is a copy of this file with
   `sources[].type: dir / path: ../..` replaced by `type: git /
   url: https://github.com/milnet01/ants-terminal / tag: v<version>`.
3. Every release updates that `tag:` line. Flathub CI rebuilds
   automatically. The
   [release-note body](../../CHANGELOG.md) mirrors into the Flathub
   listing via the AppStream metainfo `<release>` blocks installed by
   `CMakeLists.txt`.

See the main [../README.md](../README.md) for the openSUSE / Arch /
Debian recipes that sit alongside this one.
