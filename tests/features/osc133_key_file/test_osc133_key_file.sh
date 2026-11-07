#!/usr/bin/env bash
# Contract: docs/specs/ANTS-6441-osc133-key-file.md (INV-4, INV-5).
# Sources each hook with $ANTS_OSC133_KEY exported, with and without a key
# file, and checks which key signed the marker and that a child process
# inherits neither key variable.
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dir="$here/../../../packaging/shell-integration"
fail=0

command -v openssl >/dev/null 2>&1 || { echo "SKIP: openssl not installed"; exit 77; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
file_key="$(printf 'a%.0s' {1..64})"
env_key="$(printf 'b%.0s' {1..64})"
mkdir -p "$tmp/with/ants-terminal" "$tmp/without"
printf '%s\n' "$file_key" > "$tmp/with/ants-terminal/osc133.key"

# The body each shell runs: emit one A marker, then report what a child sees.
body='source "$1"; __ants_osc133_emit A; printf "\nchild:%s\n" "$(env | grep -c "^ANTS_OSC133_KEY=\|^__ants_osc133_key=")"'

check() {  # <label> <output> <expected key>
    local aid mac want
    aid="$(printf '%s' "$2" | sed -n 's/.*133;A;aid=\([^;]*\);ahmac=.*/\1/p' | head -1)"
    mac="$(printf '%s' "$2" | sed -n 's/.*;ahmac=\([0-9a-f]\{64\}\).*/\1/p' | head -1)"
    want="$(printf 'A|%s' "$aid" | openssl dgst -sha256 -hmac "$3" | sed 's/.*= *//')"
    if [ -z "$mac" ]; then
        echo "FAIL $1: no signed OSC 133;A marker in: $(printf '%s' "$2" | od -c | head -3)"; fail=1
    elif [ "$mac" != "$want" ]; then
        echo "FAIL $1: marker signed with the wrong key"; fail=1
    elif ! printf '%s' "$2" | grep -q '^child:0$'; then
        echo "FAIL $1: a child process inherits a key variable ($(printf '%s' "$2" | grep '^child:'))"; fail=1
    else
        echo "ok   $1"
    fi
}

export TERM_PROGRAM=ants-terminal ANTS_OSC133_KEY="$env_key"

run() {  # <shell> <script> <xdg dir>
    XDG_CONFIG_HOME="$3" "$1" -c "$body" _ "$2" 2>&1
}

# INV-4: the file's key signs; INV-5: no file, the env key signs. Both: no child sees a key.
check "bash file" "$(run bash "$dir/ants-osc133.bash" "$tmp/with")" "$file_key"
check "bash env"  "$(run bash "$dir/ants-osc133.bash" "$tmp/without")" "$env_key"

if command -v zsh >/dev/null 2>&1; then
    check "zsh file" "$(run zsh "$dir/ants-osc133.zsh" "$tmp/with")" "$file_key"
    check "zsh env"  "$(run zsh "$dir/ants-osc133.zsh" "$tmp/without")" "$env_key"
else
    echo "skip zsh: not installed"
fi

exit "$fail"
