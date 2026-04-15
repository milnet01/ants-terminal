#!/usr/bin/env bash
#
# check-version-drift.sh — asserts every version-bearing packaging file
# agrees with the authoritative CMakeLists.txt project(VERSION) value.
#
# Single source of truth for the audit rule `packaging_version_drift`:
#   - src/auditdialog.cpp invokes this script; the audit dialog parses the
#     stdout findings into its usual FILE:LINE: message format.
#   - .github/workflows/ci.yml invokes this script; a non-zero exit fails
#     the release PR *before* a drifted version tag ships.
#
# The ninth audit (2026-04-15, memory: audit_2026_04_15.md) flagged that
# the drift rule only ran under honor-system conventions — it missed the
# 0.6.23 release even though it existed. Extracting + wiring it into CI
# closes that loop.
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

# Packaging files — keep in sync with the release checklist in
# CLAUDE.md "Key Design Decisions" (every bump touches these). Adding
# a new packaging file? Append a check() line here and ensure the
# version-bearing pattern is regex-escaped for grep -E.
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
# head -1 after the grep captures the latest declared release. Added in
# 0.6.25 after the ninth audit noted the metainfo drift the rule missed.
check packaging/linux/org.ants.Terminal.metainfo.xml \
      '<release version="[0-9]+\.[0-9]+\.[0-9]+"' \
      'AppStream metainfo'

check README.md \
      'Current version:.*[0-9]+\.[0-9]+\.[0-9]+' \
      'README'

# POSIX exit codes max at 255; we also reserve 126/127/128+ for shell errors.
# Cap to 125 so even a repo with more than 125 drifted files still returns
# a clean "non-zero means drift" signal.
[ "$drifts" -gt 125 ] && drifts=125
exit "$drifts"
