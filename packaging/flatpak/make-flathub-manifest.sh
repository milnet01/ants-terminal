#!/usr/bin/env bash
# Generate the Flathub submission carriers from the in-repo packaging.
#
# Two modes, both writing to stdout:
#
#   make-flathub-manifest.sh [VERSION]
#       Transform the dev Flatpak manifest
#       (packaging/flatpak/za.co.antsprojectshub.AntsTerminal.yml) into a
#       Flathub-ready one: the dev `type: dir / path: ../..` source block
#       becomes a reproducible `type: git / url / tag: v<VERSION>` block,
#       plus a `commit:` pin when the tag exists locally (Flathub prefers
#       tag + commit; flatpak-builder-lint warns on tag-only). Everything
#       else — the `lua` module, `x-checker-data`, finish-args, config-opts
#       — is preserved byte-identical so the two manifests can never drift.
#
#   make-flathub-manifest.sh [VERSION] --metainfo
#       Emit the release metainfo with any unreleased "Patron RC preview"
#       channel-opener <release> placeholder stripped, so the shipped
#       stable version leads the <releases> list in the submission. (The
#       in-repo metainfo keeps the preview entry for the RC channel; the
#       Flathub copy must not.)
#
# VERSION defaults to CMakeLists.txt. See packaging/flatpak/FLATHUB.md
# for the full submission flow.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
dev="$here/za.co.antsprojectshub.AntsTerminal.yml"
metainfo="$repo/packaging/linux/za.co.antsprojectshub.AntsTerminal.metainfo.xml"

mode="manifest"
version=""
for arg in "$@"; do
    case "$arg" in
        --metainfo) mode="metainfo" ;;
        -*)         echo "error: unknown flag '$arg'" >&2; exit 2 ;;
        *)          version="$arg" ;;
    esac
done

if [[ -z "$version" ]]; then
    version=$(grep -oE 'VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+' "$repo/CMakeLists.txt" | head -n1 | awk '{print $2}')
fi
if ! [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "error: invalid version '$version'" >&2
    exit 1
fi

# ---- metainfo mode ---------------------------------------------------
if [[ "$mode" == "metainfo" ]]; then
    # Drop any <release> block that is still an unreleased channel-opener
    # placeholder (its <p> contains "opens the … preview channel"), so the
    # shipped stable leads the list. Real release blocks are untouched.
    awk '
        /<release / && !inrel { inrel=1; buf=$0 ORS; isph=($0 ~ /opens the .* preview channel/); next }
        inrel {
            buf = buf $0 ORS
            if ($0 ~ /opens the .* preview channel/) isph=1
            if ($0 ~ /<\/release>/) { if (!isph) printf "%s", buf; inrel=0 }
            next
        }
        { print }
    ' "$metainfo"
    exit 0
fi

# ---- manifest mode ---------------------------------------------------
# Commit for the tag, when it exists locally. Absent tag (e.g. an
# auto-detected base with no release cut yet) → tag-only, no error.
commit="$(git -C "$repo" rev-list -n1 "v${version}" 2>/dev/null || true)"

cat <<HEADER
# GENERATED FILE — do not edit directly.
#
# Source:   packaging/flatpak/za.co.antsprojectshub.AntsTerminal.yml
# Generator: packaging/flatpak/make-flathub-manifest.sh
# Regenerate: make-flathub-manifest.sh ${version} > <flathub-repo>/za.co.antsprojectshub.AntsTerminal.yml
#
# The only difference from the dev manifest is the ants-terminal
# source block: dev uses a local working-tree checkout, Flathub
# uses a tagged (+ commit-pinned) git revision so the Flathub CI
# checks out a reproducible source. Everything else is byte-identical.

HEADER

awk -v tag="v${version}" -v commit="$commit" '
/^[[:space:]]+-[[:space:]]*type:[[:space:]]*dir[[:space:]]*$/ {
    skip = 1
    print "      - type: git"
    print "        url: https://github.com/milnet01/ants-terminal"
    print "        tag: " tag
    if (commit != "") print "        commit: " commit
    next
}
skip && /^[[:space:]]+path:[[:space:]]*\.\.\/\.\.[[:space:]]*$/ {
    skip = 0
    next
}
skip { next }
{ print }
' "$dev"
