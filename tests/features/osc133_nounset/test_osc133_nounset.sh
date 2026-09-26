#!/usr/bin/env bash
# Contract: tests/features/osc133_nounset/spec.md (INV-1).
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dir="$here/../../../packaging/shell-integration"
fail=0

command -v openssl >/dev/null 2>&1 || { echo "SKIP: openssl not installed"; exit 77; }

check() {  # <shell-label> <output> <status>
    if [ "$3" -ne 0 ]; then
        echo "FAIL $1: emit exited $3: $2"; fail=1
    elif ! printf '%s' "$2" | grep -q $'\033\\]133;A;aid=[^;]*;ahmac=[0-9a-f]\\{64\\}\a'; then
        echo "FAIL $1: no OSC 133;A sequence in: $(printf '%s' "$2" | od -c | head -3)"; fail=1
    else
        echo "ok   $1"
    fi
}

export TERM_PROGRAM=ants-terminal ANTS_OSC133_KEY=test-key

out="$(bash -c 'set -u; source "$1"; __ants_osc133_emit A' _ "$dir/ants-osc133.bash" 2>&1)"
check bash "$out" $?

if command -v zsh >/dev/null 2>&1; then
    out="$(zsh -c 'setopt nounset; source "$1"; __ants_osc133_emit A' _ "$dir/ants-osc133.zsh" 2>&1)"
    check zsh "$out" $?
else
    echo "skip zsh: not installed"
fi

exit "$fail"
