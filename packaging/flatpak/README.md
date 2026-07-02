# Flatpak packaging — Ants Terminal

One artifact that runs on every distro. The manifest at
`za.co.antsprojectshub.AntsTerminal.yml` is ready to submit to Flathub once the first
tagged release is green through CI.

## Local build

```bash
# Required once:
flatpak install --user flathub org.kde.Platform//6.10 org.kde.Sdk//6.10 \
                               org.flatpak.Builder

# From the project root (manifest is aware of its relative `../..`):
flatpak-builder --install --user --force-clean \
    build-flatpak packaging/flatpak/za.co.antsprojectshub.AntsTerminal.yml

flatpak run za.co.antsprojectshub.AntsTerminal
```

`--install --user` drops the built app under `~/.local/share/flatpak/app/`.
Reproduce the Flathub CI lint with:

```bash
flatpak run --command=flatpak-builder-lint org.flatpak.Builder \
    manifest packaging/flatpak/za.co.antsprojectshub.AntsTerminal.yml
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

## Lua plugins

The manifest builds Lua 5.4 as an in-manifest `archive` module before
`ants-terminal`. `org.kde.Sdk//6.10` does not carry `lua54-devel` and
`flathub/shared-modules` has no Lua 5.4 entry today, so an in-manifest
module is the pragmatic path.

Build invocation: `make linux MYCFLAGS="-fPIC"` followed by
`make install INSTALL_TOP=/app`. In Lua 5.4.6+ the top-level `linux`
target is already readline-free (`src/Makefile` aliases it to
`linux-noreadline`; `linux-readline` is the readline variant, and
`make linux-noreadline` is not a valid top-level target), so the
terminal links `liblua.a` statically without pulling readline into the
sandbox and does not ship the Lua REPL binary. CMake's `FindLua`
module picks up `/app/include/lua.h` + `/app/lib/liblua.a` — no pkg
config file is needed.

Tarball-hash refresh is automated via
[flatpak-external-data-checker](https://github.com/flathub/flatpak-external-data-checker).
The `x-checker-data` stanza on the `lua` module tracks
<https://www.lua.org/ftp/> for new 5.4.x releases; Flathub CI opens a
PR against the Flathub repo with the new URL + sha256 on each point
bump. No manual hash churn.

**Success criterion:** a fresh `flatpak-builder --install --user`
build loads a plugin under `~/.var/app/za.co.antsprojectshub.AntsTerminal/config/ants-terminal/plugins/`
the same way the native-package build loads
`~/.config/ants-terminal/plugins/`. CMake's configure log prints
`Lua 5.4 found — plugin system enabled` instead of the
`plugin system disabled` fallback.

## Submitting to Flathub

1. Create a GitHub repo under the [flathub](https://github.com/flathub)
   org named `za.co.antsprojectshub.AntsTerminal` (Flathub mints it after the
   new-submission PR at `flathub/flathub` is merged).
2. The repo's `za.co.antsprojectshub.AntsTerminal.yml` is a copy of this file with
   `sources[].type: dir / path: ../..` replaced by `type: git /
   url: https://github.com/milnet01/ants-terminal / tag: v<version>`.
3. Every release updates that `tag:` line. Flathub CI rebuilds
   automatically. The
   [release-note body](../../CHANGELOG.md) mirrors into the Flathub
   listing via the AppStream metainfo `<release>` blocks installed by
   `CMakeLists.txt`.

See the main [../README.md](../README.md) for the openSUSE / Arch /
Debian recipes that sit alongside this one.
