#!/usr/bin/env bash
# Contract: tests/features/osc133_sign_helper/spec.md (INV-1 to INV-4).
# Usage: test_osc133_sign.sh <path to ants-osc133-sign>
set -uo pipefail

helper="${1:?usage: $0 <ants-osc133-sign>}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dir="$here/../../../packaging/shell-integration"
fail=0

real_openssl="$(command -v openssl)" || { echo "SKIP: openssl not installed"; exit 77; }
[ -x "$helper" ] || { echo "FAIL: helper not built: $helper"; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Two PATH dirs: one holding the helper, one holding an openssl stub that
# records each call and then does the real work.
mkdir -p "$tmp/helperbin" "$tmp/stubbin" "$tmp/tools"
# The shells get a PATH of these dirs only, so an installed helper or
# openssl elsewhere cannot leak in. The scripts need awk and nothing else.
ln -s "$(command -v awk)" "$tmp/tools/awk"
ln -s "$(cd "$(dirname "$helper")" && pwd)/$(basename "$helper")" "$tmp/helperbin/ants-osc133-sign"
cat > "$tmp/stubbin/openssl" <<EOF
#!/bin/sh
echo called >> "$tmp/openssl-calls"
exec "$real_openssl" "\$@"
EOF
chmod +x "$tmp/stubbin/openssl"

ok()   { echo "ok   $1"; }
bad()  { echo "FAIL $1"; fail=1; }

key=test-key
expect() {  # <message> → openssl's hex HMAC
    printf '%s' "$1" | "$real_openssl" dgst -sha256 -hmac "$key" -hex | awk '{print $NF}'
}

# INV-1
got="$(printf '%s' "$key" | "$helper" 'D|7|0')"
[ "$got" = "$(expect 'D|7|0')" ] && ok "INV-1 helper matches openssl" \
    || bad "INV-1 helper gave '$got'"
got="$(printf '%s\n' "$key" | "$helper" 'D|7|0')"
[ "$got" = "$(expect 'D|7|0')" ] && ok "INV-1 one trailing newline dropped" \
    || bad "INV-1 newline-terminated key gave '$got'"

# INV-2
refuses() {  # <label> <stdin> [args...]
    local label="$1" input="$2"; shift 2
    local out rc
    out="$(printf '%s' "$input" | "$helper" "$@" 2>/dev/null)"; rc=$?
    if [ "$rc" -ne 0 ] && [ -z "$out" ]; then ok "INV-2 refuses $label"
    else bad "INV-2 accepted $label (rc=$rc, out='$out')"; fi
}
refuses "no message" "$key"
refuses "empty key" "" "A|1"
refuses "key over 4 KiB" "$(head -c 4097 /dev/zero | tr '\0' k)" "A|1"

# INV-3 and INV-4, per shell.
sign_in() {  # <shell> <script> <PATH>
    rm -f "$tmp/openssl-calls"
    TERM_PROGRAM=ants-terminal ANTS_OSC133_KEY="$key" PATH="$3" "$(command -v "$1")" -c \
        'source "$1"; __ants_osc133_promptid=7; __ants_osc133_hmac D 0' _ "$2" 2>&1
}
check_shell() {  # <shell> <script>
    local sh="$1" script="$2" got
    got="$(sign_in "$sh" "$script" "$tmp/helperbin:$tmp/stubbin:$tmp/tools")"
    if [ "$got" = "$(expect 'D|7|0')" ] && [ ! -e "$tmp/openssl-calls" ]; then
        ok "INV-3 $sh signs through the helper"
    else
        bad "INV-3 $sh gave '$got', openssl called: $([ -e "$tmp/openssl-calls" ] && echo yes || echo no)"
    fi
    got="$(sign_in "$sh" "$script" "$tmp/stubbin:$tmp/tools")"
    if [ "$got" = "$(expect 'D|7|0')" ] && [ -e "$tmp/openssl-calls" ]; then
        ok "INV-4 $sh falls back to openssl"
    else
        bad "INV-4 $sh fallback gave '$got'"
    fi
}

check_shell bash "$dir/ants-osc133.bash"
if command -v zsh >/dev/null 2>&1; then
    check_shell zsh "$dir/ants-osc133.zsh"
else
    echo "skip zsh: not installed"
fi

grep -q 'ants-osc133-sign' "$dir/README.md" && ok "INV-4 README names the helper" \
    || bad "INV-4 README does not mention ants-osc133-sign"

exit "$fail"
