# Distribution packaging recipes

This directory holds ready-to-submit packaging recipes for the three
mainstream non-Flatpak distros Ants Terminal targets first. Each recipe
is **self-contained** — drop it into the build tool of your choice and
it produces a binary package without patching upstream.

> **Version alignment.** The recipes carry `0.6.20` (or newer) in lockstep
> with `CMakeLists.txt` `project(… VERSION …)`. When bumping the upstream
> release, update the version string in this file's matching recipe
> **and** run the tool-specific checksum refresh
> (`updpkgsums` for Arch, `.changes` edit via `osc vc` for openSUSE,
> `dch -i` for Debian).

## Layout

```
packaging/
├── opensuse/
│   └── ants-terminal.spec         # RPM spec for openSUSE Tumbleweed + OBS
├── archlinux/
│   └── PKGBUILD                   # AUR release recipe (ants-terminal)
├── debian/
│   ├── control                    # Source + binary package declarations
│   ├── rules                      # dh $@ --buildsystem=cmake, ninja backend
│   ├── changelog                  # Debian release history (dch -i to append)
│   ├── copyright                  # DEP-5 copyright (MIT)
│   └── source/format              # 3.0 (quilt)
├── linux/                         # Shared freedesktop.org artefacts
│   ├── org.ants.Terminal.desktop
│   ├── org.ants.Terminal.metainfo.xml
│   └── ants-terminal.1
├── completions/                   # Shell completions
│   ├── ants-terminal.bash
│   ├── _ants-terminal             # zsh
│   └── ants-terminal.fish
└── README.md                      # You are here.
```

All three recipes drive CMake, honor `DESTDIR`, and install to the same
set of paths (see the **Install footprint** table in the repo root
`README.md`) — no per-distro `%install`-time file shuffling.

---

## openSUSE — RPM spec

The spec targets **Tumbleweed** but works on Leap 15.5+ with the same
BuildRequires set. All macros used (`%cmake`, `%cmake_build`,
`%cmake_install`, `%ctest`, `%autosetup`) are core openSUSE / Fedora
macros so the file is close to portable.

### Local build

```bash
# Fetch the tarball for this version (or generate one from a checkout)
osc -A https://api.opensuse.org checkout devel:languages:misc/ants-terminal
cp packaging/opensuse/ants-terminal.spec devel:languages:misc/ants-terminal/
osc build openSUSE_Tumbleweed x86_64
```

Or outside OBS:

```bash
rpmdev-setuptree
cp packaging/opensuse/ants-terminal.spec ~/rpmbuild/SPECS/
spectool -g -R ~/rpmbuild/SPECS/ants-terminal.spec   # fetch Source0
rpmbuild -ba ~/rpmbuild/SPECS/ants-terminal.spec
```

### Submitting to OBS

openSUSE's convention keeps the release history in a separate
`ants-terminal.changes` file rather than inline in the spec. Generate
entries with:

```bash
cd devel:languages:misc/ants-terminal
osc vc
```

Then `osc commit` + `osc sr openSUSE:Factory` once the build is green.

---

## Arch Linux — AUR PKGBUILD

The PKGBUILD is the **release track** (package name `ants-terminal`,
pinned to the upstream tag). Users who want to track `main` can copy
the same file with two changes — see **git variant** below.

### Local build

```bash
cp -r packaging/archlinux /tmp/ants-build
cd /tmp/ants-build
updpkgsums                          # pin the real sha256 after the tag exists
makepkg -si                         # build, check, install
```

`makepkg --check` (or `makepkg` without `--nocheck`) runs `ctest` via
the PKGBUILD `check()` function — the same audit-rule regression suite
CI runs.

### Submitting to AUR

```bash
git clone ssh://aur@aur.archlinux.org/ants-terminal.git
cp packaging/archlinux/PKGBUILD ants-terminal/
cd ants-terminal
updpkgsums
makepkg --printsrcinfo > .SRCINFO   # required by AUR
git add PKGBUILD .SRCINFO
git commit -m "ants-terminal 0.6.20-1"
git push
```

### Git variant (`ants-terminal-git`)

For a rolling package that always builds `main`, copy `PKGBUILD` and
apply three changes:

- `pkgname=ants-terminal-git`
- Add `provides=('ants-terminal')` and `conflicts=('ants-terminal')`.
- Replace `source=()` with:
  ```bash
  source=("$pkgname::git+https://github.com/milnet01/ants-terminal.git")
  sha256sums=('SKIP')
  ```
- Add a `pkgver()` function:
  ```bash
  pkgver() {
    cd "$pkgname"
    git describe --long --tags --abbrev=7 | sed 's/^v//;s/-/.r/;s/-/./'
  }
  ```
- Swap source-dir references (`ants-terminal-$pkgver` → `$pkgname`) in
  `build()`, `check()`, `package()`.

---

## Debian — `debian/` source tree

The `debian/` tree is suitable for `debuild -uc -us`, a **Launchpad
PPA**, or an eventual **ITP → official archive** submission. It builds
with `debhelper-compat 13` and drives CMake via `dh $@
--buildsystem=cmake` with Ninja as the backend.

### Local build

```bash
# Stage the recipe alongside the source. debuild expects debian/ at the
# top level, so either symlink or copy.
cp -r packaging/debian debian
debuild -uc -us -b           # binary-only build; drops .deb one level up
```

Or, to produce a signed source package suitable for a PPA:

```bash
debuild -S
dput ppa:<your-launchpad-user>/ants-terminal ../ants-terminal_*.changes
```

### Amending the changelog

Don't edit `debian/changelog` by hand — use `dch`:

```bash
dch -v 0.6.21-1 "Upstream release."   # opens editor
```

`dch` fills in date, maintainer, and format correctly.

### Preparing for ITP

The ITP (Intent To Package) bug is filed via `reportbug wnpp` against
`wnpp` with subject `ITP: ants-terminal -- Qt6 terminal emulator with
Lua plugins and project-audit dialog`. See the Debian Developer's
Reference §5.1 for the full workflow.

---

## Shared concerns across all three recipes

- **CMake `GNUInstallDirs`** is the single source of truth for install
  paths (bindir, datadir, mandir); no recipe re-states them.
- **`ANTS_TESTS=ON`** is on by default, but each recipe re-passes it
  for clarity so a future default flip doesn't silently drop the
  audit-rule regression suite from distro CI.
- **Hardening flags** (`-fstack-protector-strong`, `-D_FORTIFY_SOURCE=3`,
  RELRO, NOW, noexecstack, PIE) are applied unconditionally in
  `CMakeLists.txt`. Distro build-hardening wrappers (`dpkg-buildflags`,
  openSUSE `%optflags`, Arch `makepkg` `CPPFLAGS`) stack cleanly on top.
- **Qt6 + Lua 5.4** are the only non-glibc runtime dependencies. Every
  other audit-dialog backend (clazy, cppcheck, semgrep) is optional and
  probed at runtime.

Distro-outreach launch (filing **intent-to-package** bugs, LWN /
Phoronix / HN announcements) is tracked separately under ROADMAP §H13.
