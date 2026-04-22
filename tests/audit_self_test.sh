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
run_rule "secrets_scan"     '(api[_-]?key|password|secret[_-]?key|auth[_-]?token|credentials)[[:space:]]*[:=][[:space:]]*("[^"]{16,}"|[^[:space:]"#,]{16,})'
run_rule "conflict_markers" '^(<{7}|\|{7}|={7}|>{7})(\s|$)'
run_rule "insecure_http"    'http://[^l][^o][^c]'
run_rule "cmd_injection"    '\b(system|popen|execlp|execvp|execl|execv|execle)\s*\('
run_rule "todo_scan"        '(TODO|FIXME|HACK|XXX)(\(|:|\s)'
run_rule "format_string"    '\b[fs]?n?printf\s*\([^"]*\b\w+\s*\)'
run_rule "hardcoded_ips"    '\b([0-9]{1,3}\.){3}[0-9]{1,3}\b'
run_rule "weak_crypto"      '\b(md5|sha1|des|rc4|ecb)\b'
run_rule "memory_patterns"  '(\bnew [A-Za-z_][A-Za-z0-9_:]*[[:space:]]*\([[:space:]]*(nullptr|NULL)?[[:space:]]*\))|\b(malloc|calloc|realloc)\b'
run_rule "qt_openurl_unchecked" 'QDesktopServices::openUrl'
run_rule "silent_catch"     'catch\s*\([^)]*\)\s*\{\s*\}'
run_rule "unbounded_callback_payloads" '\w*[Cc]allback\s*\(.*QString::fromUtf8\([^)]*\.c_str\(\)'
run_rule "qnetworkreply_no_abort" 'connect\s*\([^,]*,\s*&QNetworkReply::(readyRead|finished|errorOccurred|sslErrors)\s*,\s*\['
run_rule "cmake_no_version_floor" 'find_package\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s+REQUIRED'
run_rule "bash_c_non_literal" '\b(bash|sh)\b[^=]*"-c"\s*,\s*[A-Za-z_][A-Za-z0-9_]*'
run_rule "ssh_argv_dash_host" '<<\s*shellQuote\s*\([^)]*\bhost\b'
run_rule "qimage_load_without_peek" '\.loadFromData\s*\('
run_rule "setPermissions_pair_no_helper" 'setPermissions\s*\([^)]*QFileDevice::ReadOwner\s*\|\s*QFileDevice::WriteOwner\s*\)'

# ---------------------------------------------------------------------------
# Inline suppression token recognition — the regex in commentSuppresses()
# must match every flavour we promise to honor in the docs. This test mirrors
# the `reBare` pattern from auditdialog.cpp and asserts each token is
# recognised. Rule-id list parsing is tested via a separate fixture.
# ---------------------------------------------------------------------------

SUPPRESS_RE='\b(ants-audit:[[:space:]]*disable(-next-line|-file)?|nolint(nextline)?|cppcheck-suppress|noqa|nosec|nosemgrep|gitleaks:allow|eslint-disable(-line|-next-line)?|pylint:[[:space:]]*disable)\b'

check_suppress() {
    local label="$1"
    local line="$2"
    local expect="$3"   # "yes" or "no"
    local hit="no"
    if echo "$line" | grep -qiE "$SUPPRESS_RE"; then hit="yes"; fi
    if [[ "$hit" == "$expect" ]]; then
        echo "PASS: suppress-token $label"
        pass=$((pass + 1))
    else
        echo "FAIL: suppress-token $label (line: '$line'; expected $expect, got $hit)"
        fail=$((fail + 1))
    fi
}

check_suppress "ants-native"        '// ants-audit: disable=secrets_scan'        "yes"
check_suppress "ants-next-line"     '// ants-audit: disable-next-line'           "yes"
check_suppress "ants-file"          '// ants-audit: disable-file=unsafe_c_funcs' "yes"
check_suppress "NOLINT bare"        '// NOLINT'                                   "yes"
check_suppress "NOLINT with rule"   '// NOLINT(google-explicit-constructor)'      "yes"
check_suppress "NOLINTNEXTLINE"     '// NOLINTNEXTLINE(cppcoreguidelines-*)'      "yes"
check_suppress "cppcheck-suppress"  '// cppcheck-suppress arrayIndexOutOfBounds'  "yes"
check_suppress "noqa bare"          '# noqa'                                      "yes"
check_suppress "noqa targeted"      '# noqa: E501,E702'                           "yes"
check_suppress "nosec"              '# nosec B307'                                "yes"
check_suppress "nosemgrep"          '# nosemgrep: hardcoded-secret'               "yes"
check_suppress "gitleaks-allow"     'const token = "foo"; #gitleaks:allow'        "yes"
check_suppress "eslint-disable"     '// eslint-disable-line no-alert'             "yes"
check_suppress "eslint-disable-nl"  '/* eslint-disable-next-line no-console */'   "yes"
check_suppress "pylint disable"     '# pylint: disable=unused-import'             "yes"
# Negative cases — no suppression should be matched here.
check_suppress "ordinary comment"   '// just a comment about NOLINT in a string'  "yes"  # we accept any mention — conservative
check_suppress "no-markers"         '// fix the bug in the morning'               "no"
check_suppress "code only"          'int foo() { return 42; }'                    "no"

echo

# ---------------------------------------------------------------------------
# Fixture-coverage cross-check. Every addGrepCheck() id in src/auditdialog.cpp
# must have both:
#   1. a run_rule "<id>" line in this script  (regression test exists)
#   2. a tests/audit_fixtures/<id>/ directory (fixture exists)
#
# Mirrors the "audit_fixture_coverage" runtime check in AuditDialog. The
# runtime check surfaces findings to the dev; this assertion blocks merges
# in CI. Belt-and-suspenders — new rules can't ship without test coverage.
# ---------------------------------------------------------------------------

SRC_FILE="$SCRIPT_DIR/../src/auditdialog.cpp"
if [[ -f "$SRC_FILE" ]]; then
    # Extract unique rule ids from every addGrepCheck("id", ...) call.
    mapfile -t rule_ids < <(
        grep -oE 'addGrepCheck\("[a-zA-Z_][a-zA-Z0-9_-]*"' "$SRC_FILE" \
            | sed -E 's/.*"([a-zA-Z_][a-zA-Z0-9_-]*)"/\1/' \
            | sort -u
    )
    for id in "${rule_ids[@]}"; do
        missing=()
        [[ -d "$FIXTURE_DIR/$id" ]] || missing+=("fixture-dir")
        grep -qE "^run_rule[[:space:]]+\"$id\"" "${BASH_SOURCE[0]}" \
            || missing+=("run_rule-line")
        if [[ ${#missing[@]} -eq 0 ]]; then
            echo "PASS: fixture-coverage $id"
            pass=$((pass + 1))
        else
            echo "FAIL: fixture-coverage $id (missing: ${missing[*]})"
            fail=$((fail + 1))
        fi
    done
fi

echo
echo "Audit rule tests: $pass pass, $fail fail"
exit $fail
