#!/usr/bin/env bash
# tests/features/hook_pack/test_hooks.sh — ANTS-1252 conformance harness.
# See spec.md for the 12 assertions.
#
# Exits 0 on full pass. On failure, prints `[FAIL] ...` lines to
# stderr and exits with the count of failed assertions.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
HOOKS_DIR="$REPO_ROOT/hooks"
INSTALLER="$REPO_ROOT/tools/install-hooks.sh"

failures=0
fail() {
    failures=$((failures + 1))
    printf '[FAIL] %s\n' "$*" >&2
}
pass() {
    printf '[ ok ] %s\n' "$*"
}
skip() {
    printf '[skip] %s\n' "$*"
}

# Pre-flight: every script must exist + be executable.
for f in "$HOOKS_DIR"/ants-session-preamble.sh \
         "$HOOKS_DIR"/ants-bash-veto.sh \
         "$HOOKS_DIR"/ants-read-roadmap-veto.sh \
         "$HOOKS_DIR"/ants-drift-check.sh \
         "$HOOKS_DIR"/ants-precompact-snapshot.sh \
         "$HOOKS_DIR"/_common.sh; do
    if [ ! -f "$f" ]; then
        fail "missing: $f"
    fi
done
[ "$failures" -gt 0 ] && exit "$failures"

have_jq=0
command -v jq >/dev/null 2>&1 && have_jq=1

# ---------- INV-1: every hook exits 0 on `{}` from project root ----------
cd "$REPO_ROOT"
for f in "$HOOKS_DIR"/ants-*.sh; do
    if printf '{}\n' | timeout 5 bash "$f" >/dev/null 2>&1; then
        pass "INV-1 ${f##*/} exits 0"
    else
        fail "INV-1 ${f##*/} exited non-zero on {} stdin"
    fi
done

# ---------- INV-3: preamble ≤ 500 bytes ----------
preamble_out=$(printf '{}\n' | timeout 5 bash "$HOOKS_DIR/ants-session-preamble.sh" 2>/dev/null | wc -c)
if [ "$preamble_out" -le 500 ]; then
    pass "INV-3 preamble stdout=${preamble_out} ≤ 500"
else
    fail "INV-3 preamble stdout=${preamble_out} exceeds 500-byte cap"
fi

# ---------- INV-4 & behavioural: bash-veto reason cap + hits + bypass ----------
if [ "$have_jq" -eq 1 ]; then
    check_reason_size() {
        local label="$1" payload="$2"
        local out
        out="$(printf '%s' "$payload" | bash "$HOOKS_DIR/ants-bash-veto.sh" 2>/dev/null)"
        if [ -z "$out" ]; then
            fail "INV-4/behaviour $label produced empty output (expected block JSON)"
            return
        fi
        local reason
        reason="$(printf '%s' "$out" | jq -r '.reason // empty')"
        local n=${#reason}
        if [ "$n" -le 200 ] && [ "$n" -gt 0 ]; then
            pass "INV-4 $label reason=${n} bytes"
        else
            fail "INV-4 $label reason=${n} (must be 1..200)"
        fi
    }
    check_reason_size grep-src '{"tool_input":{"command":"grep -r foo src/"}}'
    check_reason_size git-status '{"tool_input":{"command":"git status"}}'
    check_reason_size cat-roadmap '{"tool_input":{"command":"cat ROADMAP.md | grep foo"}}'

    # Negative: unrelated command must produce empty stdout.
    if [ -z "$(printf '%s' '{"tool_input":{"command":"ls -la"}}' | bash "$HOOKS_DIR/ants-bash-veto.sh" 2>/dev/null)" ]; then
        pass "behaviour bash-veto silent on unrelated command"
    else
        fail "behaviour bash-veto fired on `ls -la`"
    fi

    # Bypass: trailing `# ants-bypass` suppresses the veto.
    bypass_out="$(printf '%s' '{"tool_input":{"command":"grep -r foo src/ # ants-bypass"}}' | bash "$HOOKS_DIR/ants-bash-veto.sh" 2>/dev/null)"
    if [ -z "$bypass_out" ]; then
        pass "behaviour bash-veto bypass strips veto"
    else
        fail "behaviour bash-veto bypass did NOT suppress: $bypass_out"
    fi
else
    skip "INV-4 + bash-veto behaviour (jq not available)"
fi

# ---------- INV-7: jq -r in source, no raw-regex on JSON ----------
for f in "$HOOKS_DIR/ants-bash-veto.sh" "$HOOKS_DIR/ants-read-roadmap-veto.sh"; do
    if grep -q "jq -r" "$f"; then
        pass "INV-7 ${f##*/} parses via jq -r"
    else
        fail "INV-7 ${f##*/} does NOT use jq -r"
    fi
    # Negative: must NOT contain `grep -P` or `sed -E .* tool_input` etc.
    # (Grep over `tool_input` is fine in comments — we only flag invocations
    # of regex tools that consume stdin.)
    if grep -E '^[^#]*(grep -P|sed -E|perl -ne).*tool_input' "$f" >/dev/null 2>&1; then
        fail "INV-7 ${f##*/} appears to regex over tool_input payload"
    else
        pass "INV-7 ${f##*/} no raw-regex over tool_input"
    fi
done

# ---------- INV-10: side-effect-only hooks emit zero stdout ----------
for f in "$HOOKS_DIR/ants-drift-check.sh" "$HOOKS_DIR/ants-precompact-snapshot.sh"; do
    bytes=$(printf '{}\n' | timeout 5 bash "$f" 2>/dev/null | wc -c)
    if [ "$bytes" -eq 0 ]; then
        pass "INV-10 ${f##*/} stdout=0"
    else
        fail "INV-10 ${f##*/} emitted ${bytes} bytes (must be 0)"
    fi
done

# ---------- INV-12: bypass token never appears in any actual reason ----------
# Runtime check (per spec INV-12): for each pattern that fires, the
# emitted block-reason MUST NOT contain `# ants-bypass`. The literal
# does appear in source — inside a `case` glob pattern that ENABLES
# the bypass; INV-12's concern is leakage to the model, i.e. into
# the reason string the model reads.
if [ "$have_jq" -eq 1 ]; then
    leak=0
    for payload in \
        '{"tool_input":{"command":"grep -r foo src/"}}' \
        '{"tool_input":{"command":"git status"}}' \
        '{"tool_input":{"command":"cat ROADMAP.md | grep foo"}}'; do
        reason="$(printf '%s' "$payload" | bash "$HOOKS_DIR/ants-bash-veto.sh" 2>/dev/null \
            | jq -r '.reason // empty' 2>/dev/null)"
        case "$reason" in
            *"# ants-bypass"*) leak=1 ;;
        esac
    done
    if [ "$leak" -eq 0 ]; then
        pass "INV-12 bypass token absent from emitted reasons"
    else
        fail "INV-12 a block reason leaked the bypass token to model"
    fi
else
    skip "INV-12 (jq not available)"
fi

# ---------- Behaviour: read-roadmap veto fires + bypasses ----------
if [ "$have_jq" -eq 1 ]; then
    out_full="$(printf '%s' "{\"tool_input\":{\"file_path\":\"$REPO_ROOT/ROADMAP.md\"}}" | bash "$HOOKS_DIR/ants-read-roadmap-veto.sh" 2>/dev/null)"
    if [ -n "$out_full" ]; then
        pass "behaviour read-roadmap-veto fires on full read"
    else
        fail "behaviour read-roadmap-veto did NOT fire on full read"
    fi
    out_offset="$(printf '%s' "{\"tool_input\":{\"file_path\":\"$REPO_ROOT/ROADMAP.md\",\"offset\":100}}" | bash "$HOOKS_DIR/ants-read-roadmap-veto.sh" 2>/dev/null)"
    if [ -z "$out_offset" ]; then
        pass "behaviour read-roadmap-veto bypassed by offset"
    else
        fail "behaviour read-roadmap-veto fired despite offset"
    fi
    out_other="$(printf '%s' '{"tool_input":{"file_path":"/etc/hosts"}}' | bash "$HOOKS_DIR/ants-read-roadmap-veto.sh" 2>/dev/null)"
    if [ -z "$out_other" ]; then
        pass "behaviour read-roadmap-veto silent on unrelated file"
    else
        fail "behaviour read-roadmap-veto fired on /etc/hosts"
    fi
else
    skip "read-roadmap-veto behaviour (jq not available)"
fi

# ---------- INV-9: silent outside project ----------
outside="$(mktemp -d -t ants-hook-pack-out.XXXXXX)"
for f in "$HOOKS_DIR"/ants-*.sh; do
    out=$(cd "$outside" && printf '{}\n' | HOME="$outside" timeout 5 bash "$f" 2>/dev/null)
    if [ -z "$out" ]; then
        pass "INV-9 ${f##*/} silent outside project"
    else
        fail "INV-9 ${f##*/} emitted output outside project: $out"
    fi
done
rm -rf "$outside"

# ---------- INV-2: sessionId validator ----------
# Source _common.sh into a subshell so we can call ants_validate_session_id.
test_sid() {
    local input="$1" expect="$2"
    local got
    got="$(bash -c ". '$HOOKS_DIR/_common.sh' && ants_validate_session_id '$input'" 2>/dev/null || true)"
    if [ "$got" = "$expect" ]; then
        pass "INV-2 sid '$input' → '$got'"
    else
        fail "INV-2 sid '$input' → '$got' (expected '$expect')"
    fi
}
test_sid "abc123_-" "abc123_-"
test_sid "../foo" ""
test_sid "a/b" ""
test_sid "" ""
# 65-char overflow:
overflow=$(printf 'a%.0s' $(seq 1 65))
test_sid "$overflow" ""

# ---------- install-hooks.sh round-trip ----------
if [ "$have_jq" -eq 1 ]; then
    tmp="$(mktemp -d -t ants-hook-install.XXXXXX)"

    # Dry-run output: `[dry-run]` log lines + a jq-pretty JSON block.
    # Extract just the JSON block (lines from the first `{` up to and
    # including the line where brace depth returns to zero), feed to
    # `jq empty`.
    dry_out="$(bash "$INSTALLER" --dry-run --target "$tmp/settings.json" --hooks-dir "$tmp/hooks" 2>/dev/null)"
    if printf '%s\n' "$dry_out" | awk '
        /^\{/ { in_json=1 }
        in_json {
            print
            for (i=1; i<=length($0); i++) {
                c = substr($0, i, 1)
                if (c == "{") depth++
                else if (c == "}") depth--
            }
            if (depth == 0 && in_json) exit
        }' | jq empty 2>/dev/null; then
        pass "install-hooks --dry-run emits parseable JSON"
    else
        fail "install-hooks --dry-run emitted unparseable JSON"
    fi

    # Live install.
    if bash "$INSTALLER" --target "$tmp/settings.json" --hooks-dir "$tmp/hooks" >/dev/null 2>&1; then
        if jq -e '.ants_hooks_pack_v1 == true' "$tmp/settings.json" >/dev/null 2>&1; then
            pass "install-hooks live install sets sentinel"
        else
            fail "install-hooks live install missing sentinel"
        fi
    else
        fail "install-hooks live install errored"
    fi

    # Idempotency.
    if bash "$INSTALLER" --target "$tmp/settings.json" --hooks-dir "$tmp/hooks" 2>&1 | grep -q "no changes"; then
        pass "install-hooks idempotent re-run"
    else
        fail "install-hooks re-run did NOT report no-op"
    fi

    # Uninstall round-trip.
    bash "$INSTALLER" --uninstall --target "$tmp/settings.json" --hooks-dir "$tmp/hooks" >/dev/null 2>&1
    if [ "$(jq -r 'has("ants_hooks_pack_v1")' "$tmp/settings.json")" = "false" ]; then
        pass "install-hooks uninstall removes sentinel"
    else
        fail "install-hooks uninstall did NOT remove sentinel"
    fi

    # INV-5 symlink abort.
    rm -f "$tmp/symlink-target"
    ln -s /etc/hosts "$tmp/symlink-target"
    if ! bash "$INSTALLER" --target "$tmp/symlink-target" --hooks-dir "$tmp/hooks" >/dev/null 2>&1; then
        pass "INV-5 install-hooks aborts on symlink target"
    else
        fail "INV-5 install-hooks did NOT abort on symlink target"
    fi

    rm -rf "$tmp"
else
    skip "install-hooks round-trip (jq not available)"
fi

if [ "$failures" -gt 0 ]; then
    printf '\n%d assertion(s) failed.\n' "$failures" >&2
    exit "$failures"
fi
echo "all hook-pack assertions passed"
exit 0
