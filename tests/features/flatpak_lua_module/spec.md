# Flatpak Lua module — feature spec

## Contract

The Flatpak manifest at `packaging/flatpak/org.ants.Terminal.yml` must
build Lua 5.4 in-manifest so `PluginManager` has the same plugin
surface inside the sandbox as the native packages. `org.kde.Sdk//6.7`
does not ship `lua54-devel` and `flathub/shared-modules` has no Lua
5.4 entry today, so an in-manifest `archive` module is the
pragmatic path.

## Invariants

**INV-1 — Lua module precedes ants-terminal.**
The `lua` module must appear in the `modules:` list **before**
`ants-terminal`. Flatpak-builder evaluates modules in order; the
ants-terminal CMake configure step must see `/app/include/lua.h` +
`/app/lib/liblua.a` already installed. Reversing the order would
silently fall through to the CMake `LUA_FOUND=FALSE` branch, producing
a Flatpak without plugin support — the exact regression this module
exists to prevent.

**INV-2 — Archive source with pinned sha256.**
The Lua module must use `type: archive` with both a `url` pointing at
`https://www.lua.org/ftp/lua-5.4.X.tar.gz` and a pinned `sha256:` field.
Flatpak refuses to build archive sources without a hash, and we refuse
to ship manifests that could pull a different tarball on different
builds.

**INV-3 — fPIC in build commands.**
`liblua.a` is statically linked into the ants-terminal executable,
which is PIE. Without `-fPIC`, the link step fails with
`requires dynamic R_X86_64_32 reloc against 'foo' which may overflow`.
The `make` invocation must pass `MYCFLAGS="-fPIC"` (the standard Lua
Makefile variable for appending compiler flags).

**INV-4 — Install to /app.**
`make install` must be invoked with `INSTALL_TOP=/app` so headers land
at `/app/include/lua.h` and the library at `/app/lib/liblua.a` —
exactly where CMake's `FindLua` module searches by default when
`CMAKE_INSTALL_PREFIX=/app`.

**INV-5 — Readline-free build target.**
Lua's default `make linux` target aliases to `linux-readline`, which
adds `-lreadline` to the lua interpreter link step. We don't ship the
Lua REPL binary (the terminal only statically links `liblua.a`), so
pulling in readline is pure bloat. The build target must be
`linux-noreadline` to keep the sandbox minimal.

**INV-6 — Tarball-hash auto-refresh.**
The Lua module must carry an `x-checker-data:` stanza compatible with
[flatpak-external-data-checker](https://github.com/flathub/flatpak-external-data-checker)
so Flathub CI auto-refreshes the URL + sha256 on each Lua 5.4.x
point release. Without this, every Lua bump is a manual sha256sum +
manifest edit + PR — churn we already pay for on the tarball itself;
don't pay for it twice.

## Why these invariants matter

- **INV-1:** module order is a silent-wrong failure. The Flatpak
  builds, installs, runs — but loading a plugin produces
  "plugin system not compiled in" with no hint as to why. A
  test-time check catches the regression at commit time.
- **INV-2:** unpinned archives are a reproducibility and
  supply-chain risk. Flatpak enforces this, but we double-check in
  case the manifest is ever refactored (e.g. someone copy-pastes
  from a `type: git` example and forgets the hash).
- **INV-3/INV-5:** both are environment-sensitive. A successful
  build against a `make linux` target with readline installed in
  the SDK would regress the day the SDK drops readline; pinning
  the flag shape prevents that.
- **INV-6:** without `x-checker-data`, the Lua tarball ages in
  place. Flathub's store listing silently lists a stale Lua
  version; security fixes in newer Lua point releases don't reach
  users.

## Test strategy

Source-grep against the manifest YAML, same as `flatpak_host_shell`.
The Flatpak toolchain isn't available on most CI runners and even
where it is, running a full `flatpak-builder` build in a feature test
would add minutes per CI run for a regression that's already
catchable by a string check.

Test binary links no Qt / no Ants sources; it reads the manifest
file as text and asserts the shape.
