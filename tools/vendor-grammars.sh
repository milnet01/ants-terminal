#!/usr/bin/env bash
# ANTS-5734 — refresh the vendored tree-sitter runtime and grammars from the
# pins in third_party/tree-sitter-grammars/MANIFEST.json.
# Contract: docs/specs/ANTS-5734-trace-verb.md § 2.3.
#
# Usage: tools/vendor-grammars.sh [--pin] [name...]
#   name    "tree-sitter" for the runtime, or a grammar's lang id; default all.
#   --pin   record the tarball hash where the manifest's sha256 is empty.
#           Without it an empty or mismatched hash refuses.
#
# Each pinned commit's tarball is fetched into a fresh temporary directory,
# its sha256 checked, and only the files the build needs are copied:
#   runtime  lib/src, lib/include, LICENSE  -> third_party/tree-sitter/
#   grammar  [subdir/]src/parser.c, src/scanner.c where present,
#            src/tree_sitter/*.h, LICENSE (or "license_file"), and any
#            "extra" paths
#            -> third_party/tree-sitter-grammars/<lang>/
# The build itself never fetches anything.

set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
manifest="$root/third_party/tree-sitter-grammars/MANIFEST.json"
grammars_dir="$root/third_party/tree-sitter-grammars"
runtime_dir="$root/third_party/tree-sitter"

pin=0
names=()
for a in "$@"; do
    case "$a" in
        --pin) pin=1 ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) names+=("$a") ;;
    esac
done

for tool in curl jq sha256sum tar; do
    command -v "$tool" >/dev/null || { echo "vendor-grammars: $tool not found" >&2; exit 2; }
done

work=$(mktemp -d "${TMPDIR:-/tmp}/vendor-grammars.XXXXXX")
trap 'rm -rf "$work"' EXIT

wanted() {
    [ ${#names[@]} -eq 0 ] && return 0
    local n
    for n in "${names[@]}"; do [ "$n" = "$1" ] && return 0; done
    return 1
}

# fetch <jq path of the entry> <name>: prints the extracted tree's top directory.
fetch() {
    local path=$1 name=$2 repo commit want got slug tarball dest
    repo=$(jq -r "$path.repo" "$manifest")
    commit=$(jq -r "$path.commit" "$manifest")
    want=$(jq -r "$path.sha256" "$manifest")
    slug=${repo#https://github.com/}
    tarball="$work/$name.tar.gz"
    curl -fsSL "https://codeload.github.com/$slug/tar.gz/$commit" -o "$tarball"
    got=$(sha256sum "$tarball" | cut -d' ' -f1)
    if [ -z "$want" ]; then
        if [ "$pin" -ne 1 ]; then
            echo "vendor-grammars: $name has no sha256; rerun with --pin" >&2
            return 1
        fi
        local tmp="$work/manifest.json"
        jq "$path.sha256 = \"$got\"" "$manifest" > "$tmp" && cp "$tmp" "$manifest"
        echo "vendor-grammars: $name pinned sha256 $got" >&2
    elif [ "$want" != "$got" ]; then
        echo "vendor-grammars: $name sha256 mismatch: manifest $want, fetched $got" >&2
        return 1
    fi
    dest="$work/$name"
    mkdir -p "$dest"
    tar -xzf "$tarball" -C "$dest" --strip-components=1
    printf '%s\n' "$dest"
}

if wanted tree-sitter; then
    src=$(fetch '.runtime' tree-sitter)
    rm -rf "$runtime_dir"
    mkdir -p "$runtime_dir/lib"
    cp -r "$src/lib/src" "$src/lib/include" "$runtime_dir/lib/"
    cp "$src/LICENSE" "$runtime_dir/LICENSE"
    echo "vendor-grammars: tree-sitter -> third_party/tree-sitter"
fi

count=$(jq '.grammars | length' "$manifest")
for ((i = 0; i < count; i++)); do
    lang=$(jq -r ".grammars[$i].lang" "$manifest")
    wanted "$lang" || continue
    src=$(fetch ".grammars[$i]" "$lang")
    sub=$(jq -r ".grammars[$i].subdir // \"\"" "$manifest")
    gsrc="$src${sub:+/$sub}/src"
    if [ ! -f "$gsrc/parser.c" ]; then
        echo "vendor-grammars: $lang has no committed src/parser.c" >&2
        exit 1
    fi
    dest="$grammars_dir/$lang"
    rm -rf "$dest"
    mkdir -p "$dest/src/tree_sitter"
    cp "$gsrc/parser.c" "$dest/src/"
    [ -f "$gsrc/scanner.c" ] && cp "$gsrc/scanner.c" "$dest/src/"
    cp "$gsrc"/tree_sitter/*.h "$dest/src/tree_sitter/"
    lic=$(jq -r ".grammars[$i].license_file // \"LICENSE\"" "$manifest")
    cp "$src/$lic" "$dest/LICENSE"
    while IFS= read -r extra; do
        [ -n "$extra" ] || continue
        mkdir -p "$grammars_dir/$(dirname "$extra")"
        cp "$src/$extra" "$grammars_dir/$extra"
    done < <(jq -r ".grammars[$i].extra // [] | .[]" "$manifest")
    abi=$(sed -n 's/^#define LANGUAGE_VERSION \([0-9]*\).*/\1/p' "$dest/src/parser.c")
    echo "vendor-grammars: $lang -> third_party/tree-sitter-grammars/$lang (ABI $abi)"
done
