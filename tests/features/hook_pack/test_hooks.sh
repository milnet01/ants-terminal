#!/usr/bin/env bash
# tests/features/hook_pack/test_hooks.sh — ANTS-1252 conformance harness,
# extended with ANTS-2141 grep/find soft-warn + throttle + counter coverage and
# ANTS-2023 cat/head/tail/bat read-dump soft-warn coverage.
# See spec.md for the assertion list.
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
cd "$REPO_ROOT" || { echo "FAIL: cd $REPO_ROOT failed"; exit 1; }
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

# ---------- Auto-compact resume flow (ANTS-3563 / ANTS-1252 §1.5) ----------
# The PreCompact snapshot → SessionStart `[ants:resume]` chain is what makes
# Claude Code's native auto-compaction LOSSLESS for an autonomous run: the
# in-progress/pending task list survives the compact so CC resumes where it
# left off. Lock the chain end-to-end so it can't silently regress.
if [ "$have_jq" -eq 1 ]; then
    RC_HOME="$(mktemp -d -t ants-resume.XXXXXX)"
    mkdir -p "$RC_HOME/.cache/ants-terminal"
    rc_sid="sess-resume-test"
    printf '%s' '{"ts":"2026-07-17T00:00:00Z","todos":[{"content":"Finish parser fix","status":"in_progress"},{"content":"Write test","status":"pending"},{"content":"Old done","status":"completed"}]}' \
        > "$RC_HOME/.cache/ants-terminal/precompact_${rc_sid}.json"
    rc_out="$(cd "$REPO_ROOT" && printf '{}\n' | CLAUDE_SESSION_ID="$rc_sid" HOME="$RC_HOME" bash "$HOOKS_DIR/ants-session-preamble.sh" 2>/dev/null)"
    if printf '%s' "$rc_out" | grep -q '\[ants:resume\].*Finish parser fix' \
       && printf '%s' "$rc_out" | grep -q 'Write test'; then
        pass "auto-compact resume: [ants:resume] surfaces in_progress+pending todos"
    else
        fail "auto-compact resume: missing/incomplete resume line: ${rc_out}"
    fi
    if printf '%s' "$rc_out" | grep -q 'Old done'; then
        fail "auto-compact resume: leaked a completed todo into the resume line"
    else
        pass "auto-compact resume: completed todos excluded"
    fi
    # No session id → no snapshot lookup → no stale resume line.
    rc_none="$(cd "$REPO_ROOT" && printf '{}\n' | CLAUDE_SESSION_ID="" HOME="$RC_HOME" bash "$HOOKS_DIR/ants-session-preamble.sh" 2>/dev/null)"
    if printf '%s' "$rc_none" | grep -q '\[ants:resume\]'; then
        fail "auto-compact resume: emitted a resume line with no session id"
    else
        pass "auto-compact resume: no resume line without a session id"
    fi
    rm -rf "$RC_HOME"
else
    skip "auto-compact resume flow (jq not available)"
fi

# ---------- ANTS-4516: installed-pack drift is reported ----------
# Two ids were shipped, tested and green while having no effect on the
# machine, for a month, because nothing re-runs the installer. The pack now
# reports its own staleness. Read-only by design: the item weighed writing
# to ~/.claude at launch against a message and chose the message.
(
    # shellcheck source=hooks/_common.sh disable=SC1091
    . "$HOOKS_DIR/_common.sh"

    # Guard the two silent-arm assertions below: a MISSING function also
    # prints nothing, so without this they would pass for the wrong reason.
    if ! command -v ants_hook_drift_line >/dev/null 2>&1; then
        fail "ANTS-4516 ants_hook_drift_line is not defined in _common.sh"
        exit 1
    fi

    drift_src="$(mktemp -d)"
    drift_dst="$(mktemp -d)"
    trap 'rm -rf "$drift_src" "$drift_dst"' EXIT

    printf '#!/bin/sh\nexit 0\n' > "$drift_src/alpha.sh"
    printf '#!/bin/sh\nexit 0\n' > "$drift_src/beta.sh"
    cp "$drift_src/alpha.sh" "$drift_dst/alpha.sh"
    cp "$drift_src/beta.sh" "$drift_dst/beta.sh"

    # In sync: silent. A warning that fires when nothing is wrong is a
    # warning nobody reads.
    if [ -z "$(ants_hook_drift_line "$drift_src" "$drift_dst")" ]; then
        pass "ANTS-4516 in-sync pack emits nothing"
    else
        fail "ANTS-4516 fired on an in-sync pack"
    fi

    # Installed copy edited behind the repo's back — the month-stale case.
    printf '#!/bin/sh\n# stale\nexit 0\n' > "$drift_dst/alpha.sh"
    stale_line="$(ants_hook_drift_line "$drift_src" "$drift_dst")"
    case "$stale_line" in
        *alpha*) pass "ANTS-4516 names the drifted hook" ;;
        "")      fail "ANTS-4516 silent on a drifted hook" ;;
        *)       fail "ANTS-4516 wrong hook named: $stale_line" ;;
    esac
    case "$stale_line" in
        *install-hooks.sh*) pass "ANTS-4516 names the command that fixes it" ;;
        *) fail "ANTS-4516 says what is wrong but not what to do: $stale_line" ;;
    esac
    case "$stale_line" in
        *beta*) fail "ANTS-4516 named an in-sync hook: $stale_line" ;;
        *)      pass "ANTS-4516 does not name an in-sync hook" ;;
    esac

    # A hook added to the pack and never installed is drift too: that is
    # exactly the never-installed shape, and cmp against a missing file
    # must not read as agreement.
    cp "$drift_src/alpha.sh" "$drift_dst/alpha.sh"
    printf '#!/bin/sh\nexit 0\n' > "$drift_src/gamma.sh"
    case "$(ants_hook_drift_line "$drift_src" "$drift_dst")" in
        *gamma*) pass "ANTS-4516 an uninstalled new hook counts as drift" ;;
        *)       fail "ANTS-4516 missed a hook absent from the install dir" ;;
    esac

    # No install dir at all: nothing to compare, so say nothing rather
    # than reporting every hook as stale.
    if [ -z "$(ants_hook_drift_line "$drift_src" "$drift_dst/absent")" ]; then
        pass "ANTS-4516 silent when there is nothing to compare against"
    else
        fail "ANTS-4516 reported drift against a non-existent install dir"
    fi
) || failures=$((failures + 1))

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
    # ANTS-2141: grep/find-over-source is now a soft-warn, NOT a block — its
    # block assertion moved to the warn-class section below. git/roadmap stay
    # block-class.
    check_reason_size git-status '{"tool_input":{"command":"git status"}}'
    check_reason_size cat-roadmap '{"tool_input":{"command":"cat ROADMAP.md | grep foo"}}'

    # Negative: unrelated command must produce empty stdout.
    if [ -z "$(printf '%s' '{"tool_input":{"command":"ls -la"}}' | bash "$HOOKS_DIR/ants-bash-veto.sh" 2>/dev/null)" ]; then
        pass "behaviour bash-veto silent on unrelated command"
    else
        fail "behaviour bash-veto fired on \"ls -la\""
    fi

    # ANTS-2169: ROADMAP.md as a grep -v / --exclude EXCLUSION is not a read of
    # the file, so the roadmap_query block must NOT fire on it.
    for excl in \
        '{"tool_input":{"command":"grep -v ROADMAP.md"}}' \
        '{"tool_input":{"command":"git diff --name-only | grep -v ROADMAP.md"}}'; do
        if [ -z "$(printf '%s' "$excl" | bash "$HOOKS_DIR/ants-bash-veto.sh" 2>/dev/null)" ]; then
            pass "ANTS-2169 no block on ROADMAP.md exclusion"
        else
            fail "ANTS-2169 blocked a ROADMAP.md exclusion: $excl"
        fi
    done
    # Positive guard: a genuine grep READ of ROADMAP.md must still block.
    if [ -n "$(printf '%s' '{"tool_input":{"command":"grep foo ROADMAP.md"}}' | bash "$HOOKS_DIR/ants-bash-veto.sh" 2>/dev/null)" ]; then
        pass "ANTS-2169 real grep-read of ROADMAP.md still blocks"
    else
        fail "ANTS-2169 real grep-read of ROADMAP.md no longer blocks (regression)"
    fi

    # A --stat diff asks for CHANGED LINES. get_git_status has no field for
    # them, so routing there is a dead end that ends in `# ants-bypass` — the
    # raw command the veto exists to prevent. The reason must name git_state.
    diff_reason="$(printf '%s' '{"tool_input":{"command":"git diff --stat"}}' \
        | bash "$HOOKS_DIR/ants-bash-veto.sh" 2>/dev/null | jq -r '.reason // empty')"
    case "$diff_reason" in
        *git_state*) pass "diff veto routes to git_state" ;;
        "")          fail "diff veto stopped blocking (regression)" ;;
        *)           fail "diff veto routes elsewhere: $diff_reason" ;;
    esac
    # ...and must NOT prescribe get_git_status, which cannot answer a diff.
    case "$diff_reason" in
        *"use mcp__ants__get_git_status"*)
            fail "diff veto still prescribes get_git_status: $diff_reason" ;;
        *)  pass "diff veto does not prescribe a status-only verb" ;;
    esac
    check_reason_size git-diff-stat '{"tool_input":{"command":"git diff --stat"}}'

    # ANTS-4517 — the diff branch matched the phrase ANYWHERE in the command,
    # so WRITING about a diff was vetoed even though the command runs no git
    # at all. Hit while editing this very veto: the rule blocked the fix to
    # itself, and again while writing these cases. The match is now anchored
    # to a command position, so the phrase inside a string, a heredoc or an
    # echo is text.
    for writing in \
        'echo "run git diff --stat for counts" >> notes.md' \
        'python3 -c "print(1) # git diff --stat"' \
        'sed -i "s/x/git diff --stat/" doc.md'
    do
        payload="$(jq -nc --arg c "$writing" '{tool_input:{command:$c}}')"
        if [ -z "$(printf '%s' "$payload" | bash "$HOOKS_DIR/ants-bash-veto.sh" 2>/dev/null)" ]; then
            pass "ANTS-4517 writing about a diff is not vetoed: $writing"
        else
            fail "ANTS-4517 veto fired on text, not an invocation: $writing"
        fi
    done

    # The over-correction the item named: anchoring to the START of the whole
    # command would stop blocking a real invocation in a later stage. A
    # pipeline's or a compound's second stage runs git exactly as the first
    # does, so each must still be vetoed.
    for realcmd in \
        'cd src && git diff --stat' \
        'true; git diff --stat' \
        'make || git diff --stat'
    do
        payload="$(jq -nc --arg c "$realcmd" '{tool_input:{command:$c}}')"
        if [ -n "$(printf '%s' "$payload" | bash "$HOOKS_DIR/ants-bash-veto.sh" 2>/dev/null)" ]; then
            pass "ANTS-4517 a later-stage invocation is still vetoed: $realcmd"
        else
            fail "ANTS-4517 anchoring went too far, missed: $realcmd"
        fi
    done

    # The status/log branch keeps its own verb — splitting the diff case out
    # must not have taken get_git_status with it.
    status_reason="$(printf '%s' '{"tool_input":{"command":"git status"}}' \
        | bash "$HOOKS_DIR/ants-bash-veto.sh" 2>/dev/null | jq -r '.reason // empty')"
    case "$status_reason" in
        *get_git_status*) pass "status veto still routes to get_git_status" ;;
        *) fail "status veto lost its verb: $status_reason" ;;
    esac

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

# ---------- ANTS-2141: grep/find soft-warn + throttle + counter ----------
if [ "$have_jq" -eq 1 ]; then
    A_HOME="$(mktemp -d -t ants-nudge.XXXXXX)"
    # nudge-veto: throttle disabled (always emit if eligible), isolated HOME.
    nveto() {
        printf '%s' "$1" | ANTS_GREP_NUDGE_THROTTLE_SEC=0 HOME="$A_HOME" \
            bash "$HOOKS_DIR/ants-bash-veto.sh" 2>/dev/null
    }
    # INV-1/INV-5/INV-12: warn-class produces a no-decision additionalContext.
    assert_warn() {
        local label="$1" out
        out="$(nveto "$2")"
        if [ -n "$(printf '%s' "$out" | jq -r '.hookSpecificOutput.additionalContext // empty')" ] \
           && [ "$(printf '%s' "$out" | jq -r '.hookSpecificOutput|has("permissionDecision")')" = "false" ] \
           && [ -z "$(printf '%s' "$out" | jq -r '.decision // empty')" ]; then
            pass "ANTS-2141 warn: $label"
        else
            fail "ANTS-2141 warn expected for $label, got: ${out:-<empty>}"
        fi
    }
    assert_empty() {
        local out; out="$(nveto "$2")"
        [ -z "$out" ] && pass "ANTS-2141 no-warn: $1" \
            || fail "ANTS-2141 expected empty for $1, got: $out"
    }

    assert_warn "grep -rn src/"        '{"tool_input":{"command":"grep -rn foo src/"}}'
    assert_warn "grep -rn pathless"    '{"tool_input":{"command":"grep -rn foo"}}'
    assert_warn "rg"                    '{"tool_input":{"command":"rg foo"}}'
    assert_warn "git grep"             '{"tool_input":{"command":"git grep TerminalGrid"}}'
    assert_warn "find src -name"       '{"tool_input":{"command":"find src -name x"}}'
    assert_warn "grep src/ | head"     '{"tool_input":{"command":"grep -rn x src/ | head"}}'

    assert_empty "piped grep"          '{"tool_input":{"command":"cmake b 2>&1 | grep error"}}'
    assert_empty "single-file grep"    '{"tool_input":{"command":"grep foo file.txt"}}'
    assert_empty "exempt /var/log"     '{"tool_input":{"command":"grep -r x /var/log"}}'
    assert_empty "exempt .log suffix"  '{"tool_input":{"command":"grep -rn x app.log"}}'
    assert_empty "non-elig find build" '{"tool_input":{"command":"find build -name x"}}'
    assert_empty "non-elig find -delete" '{"tool_input":{"command":"find . -name x -delete"}}'
    assert_empty "exempt --help"       '{"tool_input":{"command":"grep --help"}}'
    assert_empty "exempt bypass"       '{"tool_input":{"command":"grep -rn foo src/ # ants-bypass"}}'

    # INV-3: additionalContext field ≤ 400 bytes (field value, not envelope).
    ctx_bytes="$(nveto '{"tool_input":{"command":"grep -rn foo src/"}}' \
        | jq -j '.hookSpecificOutput.additionalContext' | LC_ALL=C wc -c)"
    if [ "$ctx_bytes" -gt 0 ] && [ "$ctx_bytes" -le 400 ]; then
        pass "ANTS-2141 INV-3 additionalContext=${ctx_bytes}B ≤400"
    else
        fail "ANTS-2141 INV-3 additionalContext=${ctx_bytes}B (must be 1..400)"
    fi

    # INV-6: ants_is_source_search pure predicate (in-process, return-not-exit).
    if (
        . "$HOOKS_DIR/_common.sh"
        p() { if ants_is_source_search "$1"; then echo m; else echo n; fi; }
        e=0
        [ "$(p 'grep -rn foo src/')" = m ] || e=1
        [ "$(p 'rg foo')" = m ] || e=1
        [ "$(p 'git grep x')" = m ] || e=1
        [ "$(p 'find src -name x')" = m ] || e=1
        [ "$(p 'grep foo file')" = n ] || e=1
        [ "$(p 'cmake | grep x')" = n ] || e=1
        [ "$(p 'find build -name x')" = n ] || e=1
        [ "$(p 'grep -rn foo src/ # ants-bypass')" = n ] || e=1
        exit $e
    ); then
        pass "ANTS-2141 INV-6 ants_is_source_search predicate table"
    else
        fail "ANTS-2141 INV-6 predicate table mismatch"
    fi

    # INV-7: throttle — fire→warn, immediate→suppressed, post-window→warn again.
    T_HOME="$(mktemp -d -t ants-nudge-thr.XXXXXX)"
    # Fix the throttle key explicitly — $(…) command substitution would give
    # each call a fresh subshell $PPID, defeating the throttle in-test.
    thr() {
        printf '%s' "$1" | ANTS_GREP_NUDGE_THROTTLE_SEC=2 ANTS_GREP_NUDGE_KEY=thrtest \
            HOME="$T_HOME" bash "$HOOKS_DIR/ants-bash-veto.sh" 2>/dev/null
    }
    o1="$(thr '{"tool_input":{"command":"grep -rn a src/"}}')"
    o2="$(thr '{"tool_input":{"command":"grep -rn b src/"}}')"
    touch -d "5 seconds ago" "$T_HOME"/.cache/ants-terminal/grep-nudge/*.stamp 2>/dev/null
    o3="$(thr '{"tool_input":{"command":"grep -rn c src/"}}')"
    if [ -n "$o1" ] && [ -z "$o2" ] && [ -n "$o3" ]; then
        pass "ANTS-2141 INV-7 throttle suppress+expiry"
    else
        fail "ANTS-2141 INV-7 throttle (o1=${o1:+set} o2=${o2:+set} o3=${o3:+set})"
    fi

    # INV-8: counter records every eligible match (incl. suppressed warned:false).
    cf="$T_HOME/.cache/ants-terminal/grep-nudge/count.jsonl"
    n8="$(wc -l <"$cf" 2>/dev/null || echo 0)"
    if [ "$n8" -eq 3 ] && grep -q '"warned":false' "$cf"; then
        pass "ANTS-2141 INV-8 counter logs all 3 (warned:false present)"
    else
        fail "ANTS-2141 INV-8 counter n=$n8 (expect 3, warned:false present)"
    fi
    # INV-8 cap: seed 600 → after one append, exactly 251.
    C_HOME="$(mktemp -d -t ants-nudge-cap.XXXXXX)"
    cdir="$C_HOME/.cache/ants-terminal/grep-nudge"; mkdir -p "$cdir"
    for i in $(seq 1 600); do echo "{\"s\":$i}"; done >"$cdir/count.jsonl"
    printf '%s' '{"tool_input":{"command":"grep -rn z src/"}}' \
        | ANTS_GREP_NUDGE_THROTTLE_SEC=0 HOME="$C_HOME" bash "$HOOKS_DIR/ants-bash-veto.sh" >/dev/null 2>&1
    capn="$(wc -l <"$cdir/count.jsonl")"
    [ "$capn" -eq 251 ] && pass "ANTS-2141 INV-8 cap 600→251" \
        || fail "ANTS-2141 INV-8 cap got $capn (expect 251)"

    # INV-9: fail-open — unwritable cache (regular file blocks mkdir) still warns.
    F_HOME="$(mktemp -d -t ants-nudge-fo.XXXXXX)"
    mkdir -p "$F_HOME/.cache/ants-terminal"; : >"$F_HOME/.cache/ants-terminal/grep-nudge"
    fo="$(printf '%s' '{"tool_input":{"command":"grep -rn q src/"}}' \
        | HOME="$F_HOME" bash "$HOOKS_DIR/ants-bash-veto.sh" 2>/dev/null)"
    [ -n "$fo" ] && pass "ANTS-2141 INV-9 fail-open warns on unwritable cache" \
        || fail "ANTS-2141 INV-9 fail-open did not warn"

    # INV-11: outside an ants project, no stamp/counter is written.
    O_HOME="$(mktemp -d -t ants-nudge-out.XXXXXX)"
    OUTSIDE2="$(mktemp -d -t ants-out.XXXXXX)"
    ( cd "$OUTSIDE2" && printf '%s' '{"tool_input":{"command":"grep -rn foo src/"}}' \
        | HOME="$O_HOME" bash "$HOOKS_DIR/ants-bash-veto.sh" >/dev/null 2>&1 )
    if [ ! -d "$O_HOME/.cache/ants-terminal/grep-nudge" ]; then
        pass "ANTS-2141 INV-11 no bookkeeping outside project"
    else
        fail "ANTS-2141 INV-11 cache dir created outside project"
    fi

    rm -rf "$A_HOME" "$T_HOME" "$C_HOME" "$F_HOME" "$O_HOME" "$OUTSIDE2"
else
    skip "ANTS-2141 soft-warn behaviour (jq not available)"
fi

# ---------- ANTS-2023: cat/head/tail/bat read-dump soft-warn ----------
if [ "$have_jq" -eq 1 ]; then
    R_HOME="$(mktemp -d -t ants-read.XXXXXX)"
    rveto() {
        printf '%s' "$1" | ANTS_GREP_NUDGE_THROTTLE_SEC=0 HOME="$R_HOME" \
            bash "$HOOKS_DIR/ants-bash-veto.sh" 2>/dev/null
    }
    # INV-1: warn-class produces a no-decision additionalContext.
    rwarn() {
        local out; out="$(rveto "$2")"
        if [ -n "$(printf '%s' "$out" | jq -r '.hookSpecificOutput.additionalContext // empty')" ] \
           && [ "$(printf '%s' "$out" | jq -r '.hookSpecificOutput|has("permissionDecision")')" = "false" ] \
           && [ -z "$(printf '%s' "$out" | jq -r '.decision // empty')" ]; then
            pass "ANTS-2023 warn: $1"
        else
            fail "ANTS-2023 warn expected for $1, got: ${out:-<empty>}"
        fi
    }
    rempty() {
        local out; out="$(rveto "$2")"
        [ -z "$out" ] && pass "ANTS-2023 no-warn: $1" \
            || fail "ANTS-2023 expected empty for $1, got: $out"
    }

    rwarn "cat src code file"  '{"tool_input":{"command":"cat src/terminalgrid.cpp"}}'
    rwarn "cat include header" '{"tool_input":{"command":"cat include/foo.h"}}'
    rwarn "head -N attached"   '{"tool_input":{"command":"head -50 vtparser.cpp"}}'
    rwarn "tail tests file"    '{"tool_input":{"command":"tail -100 tests/test_x.cpp"}}'
    rwarn "bat lua"            '{"tool_input":{"command":"bat src/foo.lua"}}'

    # INV-4: EXEMPT + non-eligible classes all produce empty stdout.
    rempty "exempt build-"     '{"tool_input":{"command":"cat build-fast/moc_x.cpp"}}'
    rempty "exempt /etc"       '{"tool_input":{"command":"cat /etc/hosts"}}'
    rempty "exempt .log"       '{"tool_input":{"command":"cat src/app.log"}}'
    rempty "exempt --help"     '{"tool_input":{"command":"head --help"}}'
    rempty "exempt bypass"     '{"tool_input":{"command":"cat src/foo.cpp # ants-bypass"}}'
    rempty "non-elig markdown" '{"tool_input":{"command":"cat README.md"}}'
    rempty "non-elig .txt"     '{"tool_input":{"command":"cat notes.txt"}}'
    rempty "non-elig redirect" '{"tool_input":{"command":"cat foo.cpp > bar.cpp"}}'
    rempty "non-elig heredoc"  '{"tool_input":{"command":"cat << EOF"}}'
    rempty "non-elig piped"    '{"tool_input":{"command":"cat src/foo.cpp | grep bar"}}'
    rempty "non-elig sep-arg"  '{"tool_input":{"command":"head -n 50 src/foo.cpp"}}'

    # INV-3: read-warn additionalContext field ≤ 400 bytes (field value).
    rctx="$(rveto '{"tool_input":{"command":"cat src/foo.cpp"}}' \
        | jq -j '.hookSpecificOutput.additionalContext' | LC_ALL=C wc -c)"
    if [ "$rctx" -gt 0 ] && [ "$rctx" -le 400 ]; then
        pass "ANTS-2023 INV-3 additionalContext=${rctx}B ≤400"
    else
        fail "ANTS-2023 INV-3 additionalContext=${rctx}B (must be 1..400)"
    fi

    # INV-5: ants_is_source_read pure predicate + disjoint from is_source_search.
    if (
        . "$HOOKS_DIR/_common.sh"
        r() { if ants_is_source_read "$1"; then echo m; else echo n; fi; }
        e=0
        [ "$(r 'cat src/terminalgrid.cpp')" = m ] || e=1
        [ "$(r 'head -50 vtparser.cpp')" = m ] || e=1
        [ "$(r 'bat src/foo.lua')" = m ] || e=1
        [ "$(r 'cat README.md')" = n ] || e=1
        [ "$(r 'cat src/foo.cpp | grep bar')" = n ] || e=1
        [ "$(r 'cat foo.cpp > bar.cpp')" = n ] || e=1
        [ "$(r 'cat build-fast/x.cpp')" = n ] || e=1
        [ "$(r 'cat src/foo.cpp # ants-bypass')" = n ] || e=1
        # Disjointness: no command classified by BOTH predicates.
        both() { ants_is_source_read "$1" && ants_is_source_search "$1"; }
        for c in 'cat src/foo.cpp' 'grep -rn foo src/' 'git grep x' 'find src -name x' 'rg foo'; do
            if both "$c"; then e=1; fi
        done
        exit $e
    ); then
        pass "ANTS-2023 INV-5 ants_is_source_read predicate + disjointness"
    else
        fail "ANTS-2023 INV-5 predicate/disjointness mismatch"
    fi

    # INV-6: shared per-PPID throttle — a grep warn suppresses an immediate read
    # warn; after the window the read warns. Counter logs all 3 (true/false/true).
    S_HOME="$(mktemp -d -t ants-read-thr.XXXXXX)"
    sthr() {
        printf '%s' "$1" | ANTS_GREP_NUDGE_THROTTLE_SEC=2 ANTS_GREP_NUDGE_KEY=rdtest \
            HOME="$S_HOME" bash "$HOOKS_DIR/ants-bash-veto.sh" 2>/dev/null
    }
    s1="$(sthr '{"tool_input":{"command":"grep -rn a src/"}}')"      # grep → warn
    s2="$(sthr '{"tool_input":{"command":"cat src/foo.cpp"}}')"      # read → suppressed
    touch -d "5 seconds ago" "$S_HOME"/.cache/ants-terminal/grep-nudge/*.stamp 2>/dev/null
    s3="$(sthr '{"tool_input":{"command":"cat src/bar.cpp"}}')"      # read → warn
    scf="$S_HOME/.cache/ants-terminal/grep-nudge/count.jsonl"
    sn="$(wc -l <"$scf" 2>/dev/null || echo 0)"
    if [ -n "$s1" ] && [ -z "$s2" ] && [ -n "$s3" ] && [ "$sn" -eq 3 ] \
       && [ "$(grep -c '"warned":false' "$scf")" -eq 1 ]; then
        pass "ANTS-2023 INV-6 shared throttle (grep→read) + counter 3 lines"
    else
        fail "ANTS-2023 INV-6 throttle (s1=${s1:+set} s2=${s2:+set} s3=${s3:+set} n=$sn)"
    fi

    rm -rf "$R_HOME" "$S_HOME"
else
    skip "ANTS-2023 read-dump soft-warn (jq not available)"
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
