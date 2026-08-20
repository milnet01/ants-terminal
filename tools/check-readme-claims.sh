#!/usr/bin/env bash
# ANTS-4584 — verify README.md's checkable claims against the code.
#
# README.md is the first thing a user reads and the last thing anyone
# re-reads. Its numbers drift silently because nothing derives them: the
# tool count in particular was called out in CLAUDE.md as "the one nothing
# else verifies". Each claim below has a single source of truth in the
# tree, so each is a grep rather than a memory.
#
# This checks NUMBERS, not prose. Whether the README still reads plainly
# for a non-programmer is a judgement no script makes; the pre-push hook
# raises that separately, on a cadence.
#
# Exit 0 = every claim matches. Exit 1 = at least one drifted.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

fail=0
check() {                       # check <label> <claimed> <actual> <where>
    if [ "$2" = "$3" ]; then
        printf '  ok    %-22s %s\n' "$1" "$2"
    else
        printf '  DRIFT %-22s README says %s, %s says %s\n' "$1" "$2" "$4" "$3"
        fail=1
    fi
}

# --- version banner vs the single source of truth in CMakeLists.txt -----
claim_version=$(grep -oE 'Version <strong>[0-9.]+' README.md \
                | grep -oE '[0-9.]+$' | head -1)
real_version=$(grep -oE 'project\([^)]*VERSION [0-9.]+' CMakeLists.txt \
                | grep -oE '[0-9.]+$' | head -1)
check "version" "${claim_version:-none}" "${real_version:-none}" "CMakeLists.txt"

# --- MCP tool count ----------------------------------------------------
# Registration is `<var>["name"] = "<tool>";` and the variable name varies
# (t, wsTool, ...), so the pattern keys on the assignment, not the var.
claim_tools=$(grep -oE '[0-9]+ ready-made tools' README.md \
                | grep -oE '^[0-9]+' | head -1)
real_tools=$(grep -cE '^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*\["name"\][[:space:]]*=[[:space:]]*"[a-z_]+";' \
                src/claudeintegration.cpp)
check "MCP tools" "${claim_tools:-none}" "${real_tools}" "claudeintegration.cpp"

# --- built-in colour themes -------------------------------------------
# Literal `th.name = "X";` are the built-ins; the loader's
# `th.name = obj.value(...)` is a USER theme and is deliberately not counted.
real_themes=$(grep -cE 'th\.name = "' src/themes.cpp)
for claim in $(grep -oE '[0-9]+ built-in colour themes|Pick from [0-9]+ colour schemes' README.md \
                 | grep -oE '[0-9]+'); do
    check "colour themes" "${claim}" "${real_themes}" "themes.cpp"
done

if [ "$fail" -ne 0 ]; then
    echo
    echo "README.md has drifted from the code. Fix the number, or the claim."
    exit 1
fi
echo "README claims: all verified."
