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
# The spec carries %if arms for the package names that differ per distro
# (ANTS-3727), so an RPM-based target should resolve without spec surgery. If a
# new one does not, OBS names the exact missing capability on the package's
# status page straight away, without consuming build time — read that rather
# than guessing, and add an arm for it.
#
# Mageia was tried and removed (ANTS-3727), and the reason is worth keeping so
# nobody re-adds it and rediscovers it: our spec resolves there fine — the
# blocker is _service. Its buildtime services must be installable in the target
# repo, and openSUSE:Tools cannot build obs-service-set_version for Mageia at
# all ("nothing provides python3-base", an openSUSE package name). tar and
# recompress are available for Mageia; set_version is not, and _service needs it
# to overwrite Version: from the pinned tag. That is an upstream gap, not
# something this package can patch, so Mageia needs _service restructured to
# drop the buildtime set_version before it is worth another attempt.
#
# Debian/Ubuntu are NOT just another line here either: OBS cannot build a .deb
# from a .spec, it needs debian.control + debian.rules alongside it
# (debtransform). Arch needs a PKGBUILD for the same reason. Both are real work,
# not a repo block. See packaging/obs/README.md § "Adding a distribution".
#
# The <debuginfo> flag below is load-bearing (ANTS-3729). Left off, OBS invokes
# rpmbuild with `_enable_debug_packages` undefined, so nothing extracts the
# symbols and nothing strips the binary: the shipped /usr/bin/ants-terminal went
# out carrying its whole symbol table (13.6 MB installed) and rpmlint reported
# unstripped-binary-or-object. It is a project-meta flag — not a prjconf line,
# not a spec change — and openSUSE:Factory sets the same one. The resulting
# -debuginfo / -debugsource packages have real content rather than being empty,
# because %optflags carries -g and CMakeLists.txt overrides neither
# CMAKE_CXX_FLAGS nor any strip setting.
#
# Keep the XML free of double hyphens. They are illegal inside an XML comment,
# so quoting an rpmbuild flag there makes OBS reject the whole meta with a
# "Double hyphen within comment" validation error. Explanations live out here.
# ---------------------------------------------------------------------------
cat > "$tmp/prj.xml" <<EOF
<project name="$PROJ">
  <title>Ants Terminal</title>
  <description>Qt6/C++20 terminal emulator with a VT100/xterm parser (Kitty keyboard and graphics, Sixel, OSC 8 hyperlinks, OSC 133 shell integration), an optional OpenGL glyph-atlas renderer, a Lua 5.4 plugin system with a sandboxed ants.* API, and a built-in Project Audit dialog with SARIF export.</description>
  <person userid="$USER" role="maintainer"/>
  <debuginfo>
    <enable/>
  </debuginfo>
  <repository name="openSUSE_Tumbleweed">
    <path project="openSUSE:Factory" repository="snapshot"/>
    <arch>x86_64</arch>
  </repository>
  <repository name="openSUSE_Leap_16.0">
    <path project="openSUSE:Leap:16.0" repository="standard"/>
    <arch>x86_64</arch>
  </repository>
  <repository name="Fedora_44">
    <path project="Fedora:44" repository="standard"/>
    <arch>x86_64</arch>
  </repository>
</project>
EOF
echo ">>> applying project meta: $PROJ"
osc -A "$API" meta prj "$PROJ" -F "$tmp/prj.xml"

# ---------------------------------------------------------------------------
# Project config (prjconf). Distinct from the meta above: the meta says WHAT to
# build, this says HOW to resolve it. Authored here for the same reason the
# <debuginfo> flag is (ANTS-3729) — a setting that lives only in the web UI is
# one nobody can see in review and that a re-run of this script silently drops.
#
# Fedora has no bare `wget` package: wget1-wget and wget2-wget are both shims
# that Provide it, so a dependency on `wget` is ambiguous and the whole job goes
# unresolvable. obs-service-download_files pulls one in, which is how a service
# we do not even call in _service ends up failing the build. `Prefer` picks the
# winner; wget2 is Fedora's current default.
#
# Scope each rule to its repository. An unscoped Prefer applies to every target,
# including openSUSE, where those package names do not exist at all.
# ---------------------------------------------------------------------------
cat > "$tmp/prjconf.txt" <<'EOF'
%if "%_repository" == "Fedora_44"
Prefer: wget2-wget
%endif
EOF
echo ">>> applying project config: $PROJ"
osc -A "$API" meta prjconf "$PROJ" -F "$tmp/prjconf.txt"

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
