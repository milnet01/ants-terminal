#!/usr/bin/env bash
# ANTS-5617 INV-1 — install the demoreel component into a scratch prefix and
# run the installed copy. Args: <build dir> <expected version>.
set -euo pipefail

build="$1"
want="demoreel $2"

command -v python3 >/dev/null || { echo "SKIP: no python3"; exit 77; }

prefix="$(mktemp -d "${build}/demoreel-bundle.XXXXXX")"
trap 'rm -rf "$prefix"' EXIT

cmake --install "$build" --component demoreel --prefix "$prefix" >/dev/null

bin="$(find "$prefix" -path '*/ants-terminal/demoreel' -type f)"
[ -n "$bin" ] || { echo "FAIL: no ants-terminal/demoreel under $prefix"; exit 1; }
case "$bin" in
    */libexec/ants-terminal/demoreel) ;;
    *) echo "FAIL: installed outside libexec: $bin"; exit 1 ;;
esac
[ -x "$bin" ] || { echo "FAIL: $bin is not executable"; exit 1; }

got="$("$bin" --version)"
[ "$got" = "$want" ] || { echo "FAIL: --version printed '$got', want '$want'"; exit 1; }
echo "PASS: $bin prints '$got'"
