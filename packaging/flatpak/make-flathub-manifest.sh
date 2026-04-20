#!/usr/bin/env bash
# Generate a Flathub-ready manifest from the development manifest.
#
# The dev manifest (packaging/flatpak/org.ants.Terminal.yml) uses
#     - type: dir
#       path: ../..
# so local flatpak-builder runs pick up the working tree. The Flathub
# repo must point at a tagged git revision instead. This script reads
# the dev manifest, replaces that single source block with
#     - type: git
#       url: https://github.com/milnet01/ants-terminal
#       tag: v<VERSION>
# and writes the result to stdout. Everything else — the `lua` module,
# `x-checker-data` stanza, finish-args, config-opts — is preserved
# byte-identical so the two manifests can never drift.
#
# Usage:
#     make-flathub-manifest.sh [VERSION]
#
# If VERSION is omitted, it is read from CMakeLists.txt. See
# packaging/flatpak/FLATHUB.md for the full submission flow.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
dev="$here/org.ants.Terminal.yml"

version="${1:-}"
if [[ -z "$version" ]]; then
    version=$(grep -oE 'VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+' "$repo/CMakeLists.txt" | head -n1 | awk '{print $2}')
fi
if ! [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "error: invalid version '$version'" >&2
    exit 1
fi

cat <<HEADER
# GENERATED FILE — do not edit directly.
#
# Source:   packaging/flatpak/org.ants.Terminal.yml
# Generator: packaging/flatpak/make-flathub-manifest.sh
# Regenerate: make-flathub-manifest.sh ${version} > <flathub-repo>/org.ants.Terminal.yml
#
# The only difference from the dev manifest is the ants-terminal
# source block: dev uses a local working-tree checkout, Flathub
# uses a tagged git revision so the Flathub CI checks out a
# reproducible source. Everything else is byte-identical.

HEADER

awk -v tag="v${version}" '
/^[[:space:]]+-[[:space:]]*type:[[:space:]]*dir[[:space:]]*$/ {
    skip = 1
    print "      - type: git"
    print "        url: https://github.com/milnet01/ants-terminal"
    print "        tag: " tag
    next
}
skip && /^[[:space:]]+path:[[:space:]]*\.\.\/\.\.[[:space:]]*$/ {
    skip = 0
    next
}
skip { next }
{ print }
' "$dev"
