#!/usr/bin/env bash
#
# check-version-drift.sh — asserts every version-bearing packaging file
# agrees with the authoritative CMakeLists.txt project(VERSION) value.
#
# Single source of truth for the audit rule `packaging_version_drift`. The
# audit dialog, CI, release.sh, a CMake test and others all run it and read
# its output and exit code; search the tree for `check-version-drift` to
# list them before changing either.
#
# Output format per finding (stdout):
#   <file>:<line>: <label> version <got> drifts from CMakeLists.txt <truth>
#
# Exit code:
#   0   — no drifts detected (or CMakeLists.txt missing / unparseable:
#         silent no-op, matches the rule's original behavior)
#   N>0 — N files drifted (capped at 125 to stay inside POSIX exit-code range)
#
# No args. Runs from the project root — the audit dialog sets its
# QProcess cwd to m_projectPath, and CI runs it after `actions/checkout`.

set -u

cml="CMakeLists.txt"
[ -f "$cml" ] || exit 0

truth=$(grep -oE 'project\s*\([^)]*VERSION\s+[0-9]+\.[0-9]+\.[0-9]+' "$cml" \
        | grep -oE '[0-9]+\.[0-9]+\.[0-9]+$' | head -1)
[ -z "$truth" ] && exit 0

drifts=0

# A MISS IS SILENT: a file that is absent, or a pattern that matches nothing
# in it, counts as no drift. The audit runs this in projects that ship only
# some of these files. So a wrong pattern looks exactly like a pass; after
# adding a check, change the file's version and see the drift reported.
check() {
    local file=$1 pattern=$2 label=$3
    [ -f "$file" ] || return
    local got
    got=$(grep -oE "$pattern" "$file" | head -1)
    [ -z "$got" ] && return
    local v
    v=$(echo "$got" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
    [ -z "$v" ] && return
    if [ "$v" != "$truth" ]; then
        local lineno
        lineno=$(grep -nE "$pattern" "$file" | head -1 | cut -d: -f1)
        printf '%s:%s: %s version %s drifts from CMakeLists.txt %s\n' \
            "$file" "$lineno" "$label" "$v" "$truth"
        drifts=$((drifts + 1))
    fi
}

# One check per file the version bump rewrites; .claude/bump.json is that
# list. Adding a packaging file? Add it there and append a check() line
# here, its version-bearing pattern regex-escaped for grep -E.
check packaging/opensuse/ants-terminal.spec \
      'Version:\s+[0-9]+\.[0-9]+\.[0-9]+' \
      'openSUSE spec'

check packaging/archlinux/PKGBUILD \
      'pkgver=[0-9]+\.[0-9]+\.[0-9]+' \
      'Arch PKGBUILD'

check packaging/debian/changelog \
      'ants-terminal \([0-9]+\.[0-9]+\.[0-9]+' \
      'Debian changelog'

check packaging/linux/ants-terminal.1 \
      'ants-terminal [0-9]+\.[0-9]+\.[0-9]+' \
      'Man page'

# AppStream metainfo lists <release version="X.Y.Z"> entries newest-first;
# head -1 after the grep captures the latest declared release.
check packaging/linux/za.co.antsprojectshub.AntsTerminal.metainfo.xml \
      '<release version="[0-9]+\.[0-9]+\.[0-9]+"' \
      'AppStream metainfo'

# README's header banner reads `Version <strong>X.Y.Z</strong>`.
check README.md \
      'Version <strong>[0-9]+\.[0-9]+\.[0-9]+' \
      'README'

# ROADMAP.md is deliberately NOT checked, and must not be re-added
# (ANTS-4529). It is generated from the roadmap store, whose render replays
# the root section's stored intro verbatim — so the bump's markdown edit was
# discarded by the next roadmap_log write and this check then re-opened drift
# on a release that had looked clean. The banner no longer states a version:
# the number duplicated CMakeLists.txt's project(VERSION), and the sentence
# that held it already links CHANGELOG.md. ANTS-2163 added the check when the
# banner was hand-maintained; nothing hand-maintains it now.

# POSIX exit codes max at 255; we also reserve 126/127/128+ for shell errors.
# Cap to 125 so even a repo with more than 125 drifted files still returns
# a clean "non-zero means drift" signal.
[ "$drifts" -gt 125 ] && drifts=125
exit "$drifts"
