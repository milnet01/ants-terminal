#!/bin/sh
# obs-setup.sh (ANTS-3726) — create/update the OBS sub-project + package.
#
# Idempotent: re-running just re-applies the meta, so this is ALSO how you add
# or remove a build target. Edit the repository list below and re-run.
#
# Ants Terminal lives in its OWN sub-project (home:milnet:ants-terminal) rather
# than as a package inside home:milnet, matching home:milnet:finbreak. That
# keeps its repository list independent — adding a distro here cannot disturb
# OneUp (home:milnet) or finbreak (home:milnet:finbreak), which define their own.
#
# Needs: osc, and an authenticated OBS account (run any osc command once to log
# in). Override the defaults via env: OBS_API, OBS_PROJECT, OBS_PACKAGE, OBS_USER.
set -eu

API="${OBS_API:-https://api.opensuse.org}"
PROJ="${OBS_PROJECT:-home:milnet:ants-terminal}"
PKG="${OBS_PACKAGE:-ants-terminal}"
USER="${OBS_USER:-milnet}"

command -v osc >/dev/null 2>&1 || { echo "obs-setup: osc not installed" >&2; exit 1; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# ---------------------------------------------------------------------------
# Build targets. THIS IS THE "add more distros" KNOB — add a <repository> block
# and re-run this script.
#
# x86_64 only for now: the package is a Qt6 desktop app, and i586 Qt6 is not a
# target anyone installs. Factory ARM/RISCV/PowerPC/zSystems are viable later
# (same spec, different arch) once x86_64 is proven.
#
# Before adding a non-openSUSE target, read packaging/obs/README.md § "Adding a
# distribution" — the spec currently carries two openSUSE-only BuildRequires
# (ANTS-3727) that make Fedora/Mageia unresolvable until they are conditional.
# Debian/Ubuntu need debian.control + debian.rules as well as the spec; OBS
# cannot build a .deb from a .spec alone.
# ---------------------------------------------------------------------------
cat > "$tmp/prj.xml" <<EOF
<project name="$PROJ">
  <title>Ants Terminal</title>
  <description>Qt6/C++20 terminal emulator with a VT100/xterm parser (Kitty keyboard and graphics, Sixel, OSC 8 hyperlinks, OSC 133 shell integration), an optional OpenGL glyph-atlas renderer, a Lua 5.4 plugin system with a sandboxed ants.* API, and a built-in Project Audit dialog with SARIF export.</description>
  <person userid="$USER" role="maintainer"/>
  <repository name="openSUSE_Tumbleweed">
    <path project="openSUSE:Factory" repository="snapshot"/>
    <arch>x86_64</arch>
  </repository>
</project>
EOF
echo ">>> applying project meta: $PROJ"
osc -A "$API" meta prj "$PROJ" -F "$tmp/prj.xml"

cat > "$tmp/pkg.xml" <<EOF
<package name="$PKG" project="$PROJ">
  <title>Qt6 terminal emulator with Lua plugins and a project-audit dialog</title>
  <description>Ants Terminal, built from the upstream release tag via obs_scm.</description>
  <person userid="$USER" role="maintainer"/>
</package>
EOF
echo ">>> applying package meta: $PROJ/$PKG"
osc -A "$API" meta pkg "$PROJ" "$PKG" -F "$tmp/pkg.xml"

echo "OK — $PROJ/$PKG ready. Next: packaging/obs/obs-submit.sh"
