#!/usr/bin/env bash
#
# audit_self_test.sh — regression test for audit rule patterns.
#
# For each rule we run the rule's grep pattern against a fixture directory
# and assert:
#   1. bad.* produces N matches, where N is the count of `@expect <rule-id>`
#      markers in bad.*.
#   2. good.* produces 0 matches (false-positive canary).
#
# Keep the rule patterns in sync with addGrepCheck() calls in
# src/auditdialog.cpp. When a pattern changes there, update this script.
# CTest runs us after every build so drift is caught immediately.
#
# Exits with the number of failed rules.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIXTURE_DIR="$SCRIPT_DIR/audit_fixtures"

pass=0
fail=0

count_matches() {
    # Total line matches across all given files (handles single- vs multi-file
    # grep -c output shapes by summing the trailing number on each line).
    local pattern="$1"; shift
    grep -cE "$pattern" "$@" 2>/dev/null | awk -F: '{sum+=$NF} END{print sum+0}'
}

run_rule() {
    local id="$1"
    local pattern="$2"
    local dir="$FIXTURE_DIR/$id"

    if [[ ! -d "$dir" ]]; then
        echo "SKIP: $id (fixture dir missing)"
        return
    fi

    local bad_files=("$dir"/bad.*)
    local good_files=("$dir"/good.*)

    # Expected = @expect markers in bad.*. Rule id is followed by EOL, colon,
    # or whitespace (allows "@expect foo" and "@expect foo:3" both to match).
    local expected
    expected=$(count_matches "@expect[[:space:]]+${id}([[:space:]:]|\$)" "${bad_files[@]}")

    local actual
    actual=$(count_matches "$pattern" "${bad_files[@]}")

    local false_positives=0
    if [[ -e "${good_files[0]}" ]]; then
        false_positives=$(count_matches "$pattern" "${good_files[@]}")
    fi

    if [[ "$expected" -eq "$actual" && "$false_positives" -eq 0 ]]; then
        echo "PASS: $id ($actual match(es) in bad, 0 in good)"
        pass=$((pass + 1))
    else
        echo "FAIL: $id"
        echo "  bad.*:  expected $expected, got $actual match(es)"
        echo "  good.*: expected 0, got $false_positives false positive(s)"
        if [[ "$false_positives" -gt 0 ]]; then
            grep -nE "$pattern" "${good_files[@]}" 2>/dev/null | sed 's/^/    /'
        fi
        fail=$((fail + 1))
    fi
}

# ---------------------------------------------------------------------------
# Rule patterns — MUST match addGrepCheck() invocations in auditdialog.cpp
# (strip the outer quotes present in the C++ source; grep -E gets the bare
# regex on argv).
# ---------------------------------------------------------------------------

run_rule "unsafe_c_funcs"   '\b(strcpy|strcat|sprintf|vsprintf|gets|mktemp|tmpnam|scanf)\s*\('
run_rule "secrets_scan"     '(api[_-]?key|password|secret[_-]?key|auth[_-]?token|credentials)\s*[:=]'
run_rule "conflict_markers" '^(<{7}|>{7}|={7})'
run_rule "insecure_http"    'http://[^l][^o][^c]'
run_rule "cmd_injection"    '\b(system|popen|execlp|execvp|execl|execv|execle)\s*\('

echo
echo "Audit rule tests: $pass pass, $fail fail"
exit $fail
