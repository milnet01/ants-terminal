#!/bin/bash
# ANTS-6656 — tools/check-roadmap.sh's --allow list exempts exactly the ids
# it names. See spec.md.
#
# Runs the real script against a throwaway roadmap file.

set -uo pipefail

SCRIPT="$(cd "$(dirname "$0")/../../.." && pwd)/tools/check-roadmap.sh"
command -v awk >/dev/null 2>&1 || { echo "SKIP: awk not available"; exit 77; }

failures=0
check() {  # check <description> <condition-result>
    if [[ "$2" == "0" ]]; then
        echo "  ok   — $1"
    else
        echo "  FAIL — $1"
        failures=$((failures + 1))
    fi
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Two duplicated ids. ANTS-11 is what a widened ANTS-1 pattern would catch.
cat > "$tmp/ROADMAP.md" <<'EOF'
- 📋 [ANTS-11] one
- 📋 [ANTS-11] two
- 📋 [ANTS-2] three
- 📋 [ANTS-2] four
EOF

run() {  # run <allow-csv>; sets $out and $rc
    out=$(timeout 10 bash "$SCRIPT" "$tmp/ROADMAP.md" --allow "$1" 2>&1)
    rc=$?
}

echo "INV-1 — a + in an allowed id is literal"
run 'ANTS-1+'
check "exit 1" "$([[ $rc -eq 1 ]] && echo 0 || echo 1)"
check "ANTS-11 is still reported" \
      "$(grep -qF '[ANTS-11]' <<<"$out" && echo 0 || echo 1)"

echo "INV-2 — a . in an allowed id is literal"
run 'ANTS-1.'
check "ANTS-11 is still reported" \
      "$(grep -qF '[ANTS-11]' <<<"$out" && echo 0 || echo 1)"

echo "INV-3 — an unbalanced ( in an allowed id is not a regex error"
run 'ANTS-(2'
check "exit 1 (duplicates), not 2 (setup error)" \
      "$([[ $rc -eq 1 ]] && echo 0 || echo 1)"
check "both duplicates are reported" \
      "$(grep -qF '[ANTS-11]' <<<"$out" && grep -qF '[ANTS-2]' <<<"$out" \
         && echo 0 || echo 1)"

echo "INV-4 — the ids it names are exempt"
run 'ANTS-2'
check "ANTS-2 is exempt" \
      "$(grep -qF '[ANTS-2]' <<<"$out" && echo 1 || echo 0)"
check "ANTS-11 is still reported" \
      "$(grep -qF '[ANTS-11]' <<<"$out" && echo 0 || echo 1)"
run 'ANTS-11,ANTS-2'
check "both named: exit 0" "$([[ $rc -eq 0 ]] && echo 0 || echo 1)"
run ''
check "an empty list exempts nothing: exit 1" \
      "$([[ $rc -eq 1 ]] && echo 0 || echo 1)"

if [[ $failures -gt 0 ]]; then
    echo "FAILED: $failures assertion(s)"
    exit 1
fi
echo "PASS"
exit 0
