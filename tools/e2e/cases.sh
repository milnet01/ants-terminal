#!/usr/bin/env bash
# ANTS-2050 — user-level feature case suite, run through the ANTS-2049 harness.
#
# Each case is drive → observe → assert against a throwaway --e2e instance
# (tools/e2e/run.sh). Cases are grouped into LANES (one function per subsystem)
# so a partial run can target a single lane:
#
#     bash tools/e2e/cases.sh              # every automated lane
#     bash tools/e2e/cases.sh terminal     # just the terminal-basics lane
#     bash tools/e2e/cases.sh resize theme # a subset
#
# Only the lanes the current harness can OBSERVE headlessly live here (terminal
# content via get-text, liveness via tab-list, resize echo, a rendered PNG via
# grab-image). The full feature checklist — including the manual / on-screen /
# pending-hook cases these four lanes don't cover — is docs/qa/e2e/cases.md;
# harness-contract guards (bad widget, gate, grab escape) are tools/e2e/smoke.sh
# and are NOT re-tested here (no duplication).
#
# NO build step (INV-7) — runs an already-built binary; ANTS_E2E_BIN selects it.
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
# shellcheck source=tools/e2e/run.sh
source tools/e2e/run.sh

fails=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; fails=$((fails + 1)); }
# has HAYSTACK NEEDLE — plain-substring match (needle is not a regex).
has()  { grep -qF "$2" <<<"$1"; }

# send_cmd TAG SHELL_LINE — type a shell command into the instance's PTY and
# press Enter. SHELL_LINE is a raw command (single-quote the bash literal to
# keep $(...) / $((...)) / escapes like \033 literal for the *remote* shell);
# it is JSON-escaped here (\ and ") before the Enter (\n) is appended.
send_cmd() {
    local tag="$1" line="$2"
    line="${line//\\/\\\\}"    # backslash  -> \\   (so \033 reaches the shell)
    line="${line//\"/\\\"}"    # dquote     -> \"
    call_e2e "{\"cmd\":\"send-text\",\"text\":\"${line}\\n\"}" "$tag" >/dev/null
}

# ── Lane: terminal basics ─────────────────────────────────────────────────
# Typing, shell output, ANSI SGR (colour codes consumed, text intact),
# carriage-return overwrite, UTF-8 round-trip, long-line integrity.
lane_terminal() {
    echo "── lane: terminal ──"
    launch_e2e terminal || { fail "terminal: launch"; return; }
    local t

    # echo + shell arithmetic.
    send_cmd terminal 'echo TB_$((6*7))_END'
    sleep 1
    t=$(call_e2e '{"cmd":"get-text"}' terminal)
    has "$t" 'TB_42_END' && pass "terminal: echo + arithmetic" \
        || fail "terminal: echo/arithmetic sentinel absent"

    # ANSI SGR: the colour escape is consumed by the parser, the text survives.
    send_cmd terminal "printf '\\033[1;31mCOLORSAFE\\033[0m\\n'"
    sleep 1
    t=$(call_e2e '{"cmd":"get-text"}' terminal)
    has "$t" 'COLORSAFE' && pass "terminal: ANSI SGR text intact" \
        || fail "terminal: ANSI SGR sentinel absent"

    # Carriage-return overwrite: 'XXXX\rYY' → the row reads 'YYXX'.
    send_cmd terminal "printf 'XXXX\\rYY\\n'"
    sleep 1
    t=$(call_e2e '{"cmd":"get-text"}' terminal)
    has "$t" 'YYXX' && pass "terminal: CR overwrite" \
        || fail "terminal: CR overwrite (expected YYXX)"

    # UTF-8 round-trip: literal multibyte bytes survive PTY→parser→grid→get-text.
    # A CJK char is double-width — get-text pads its second cell with a space
    # (中文 → "中 文 "), so assert each codepoint is present, not contiguous.
    send_cmd terminal 'echo CJK_中文_END'
    sleep 1
    t=$(call_e2e '{"cmd":"get-text"}' terminal)
    { has "$t" '中' && has "$t" '文'; } && pass "terminal: UTF-8/CJK round-trip" \
        || fail "terminal: UTF-8 bytes not observed"

    # Long-line integrity: a 200-char line survives without truncation. It wraps
    # across rows, so count the W's rather than expecting one contiguous run
    # (get-text joins wrapped rows with a newline). ≥200 = nothing dropped (the
    # extra one is the literal W in the echoed printf command).
    send_cmd terminal "printf 'W%.0s' \$(seq 1 200); printf '\\n'"
    sleep 1
    local wc; wc=$(call_e2e '{"cmd":"get-text","lines":60}' terminal | tr -cd 'W' | wc -c)
    (( wc >= 200 )) && pass "terminal: 200-char long line intact (W count=$wc)" \
        || fail "terminal: long line truncated (W count=$wc, want ≥200)"
}

# ── Lane: scrollback ──────────────────────────────────────────────────────
# History is retained beyond one screen: top + bottom markers of a 300-line
# burst are both observable through get-text.
lane_scrollback() {
    echo "── lane: scrollback ──"
    launch_e2e scrollback || { fail "scrollback: launch"; return; }

    send_cmd scrollback 'echo SCROLLTOP; seq 1 300; echo SCROLLBOT'
    sleep 2
    local t; t=$(call_e2e '{"cmd":"get-text","lines":400}' scrollback)
    if has "$t" 'SCROLLTOP' && has "$t" 'SCROLLBOT'; then
        pass "scrollback: 300-line history retained (both markers)"
    else
        fail "scrollback: a boundary marker fell out of history"
    fi
}

# ── Lane: resize ──────────────────────────────────────────────────────────
# resize-window echoes the applied (post-clamp) size; a tiny request clamps
# rather than breaks; the PTY still echoes after a reflow.
lane_resize() {
    echo "── lane: resize ──"
    launch_e2e resize || { fail "resize: launch"; return; }

    local r; r=$(call_e2e '{"cmd":"resize-window","w":1000,"h":700}' resize)
    { has "$r" '"ok":true' && has "$r" '"h":700'; } \
        && pass "resize: echoes applied {w,h}" || fail "resize: reply=$r"

    # Tiny request clamps to the minimum window rather than failing.
    r=$(call_e2e '{"cmd":"resize-window","w":10,"h":10}' resize)
    has "$r" '"ok":true' && pass "resize: tiny request clamps (ok)" \
        || fail "resize: tiny clamp reply=$r"

    # PTY still live after the reflow.
    send_cmd resize 'echo AFTER_RESIZE_OK'
    sleep 1
    has "$(call_e2e '{"cmd":"get-text"}' resize)" 'AFTER_RESIZE_OK' \
        && pass "resize: PTY echoes after reflow" \
        || fail "resize: PTY silent after reflow"
}

# ── Lane: theme (restyle liveness / stress) ───────────────────────────────
# Thrash the theme via config-reload across every built-in theme and assert the
# app survives the repeated qApp->setStyleSheet re-polish walk, then still
# renders (non-zero PNG). NOTE: this drives the config-reload applyTheme path,
# NOT the View-menu QAction path — the ANTS-3556 Wayland-menu race is guarded
# by the source-scrape test tests/features/theme_switch_popup_defer. This lane
# is general restyle-liveness coverage, not that specific race.
lane_theme() {
    echo "── lane: theme ──"
    launch_e2e theme || { fail "theme: launch"; return; }

    local cfg; cfg="$(dirname "$(e2e_art theme)")/cfg/ants-terminal/config.json"
    local th
    for th in Light Nord Dracula "Solarized Dark" Dark Light Nord Dracula Dark; do
        printf '{"remote_control_enabled": true, "theme": "%s"}\n' "$th" > "$cfg"
        sleep 0.15
    done
    sleep 0.5

    has "$(call_e2e '{"cmd":"tab-list"}' theme)" '"ok":true' \
        && pass "theme: app alive after 9-theme thrash" \
        || fail "theme: app died during theme thrash"

    local png; png="$(e2e_art theme)/after-thrash.png"
    call_e2e '{"cmd":"grab-image","path":"'"$png"'"}' theme >/dev/null
    [[ -s "$png" ]] && pass "theme: renders after thrash (non-zero PNG)" \
        || fail "theme: grab produced no PNG"
}

# ── Dispatcher ────────────────────────────────────────────────────────────
run_lane() {
    case "$1" in
        terminal)   lane_terminal ;;
        scrollback) lane_scrollback ;;
        resize)     lane_resize ;;
        theme)      lane_theme ;;
        *) echo "cases.sh: unknown lane '$1' (terminal|scrollback|resize|theme)" >&2
           fails=$((fails + 1)) ;;
    esac
}

lanes=(terminal scrollback resize theme)
[[ $# -ge 1 ]] && lanes=("$@")
for L in "${lanes[@]}"; do run_lane "$L"; done
teardown_e2e   # EXIT trap also runs harmlessly at exit

echo
if [[ "$fails" == 0 ]]; then
    echo "e2e cases: ALL PASS (lanes: ${lanes[*]})"
    exit 0
fi
echo "e2e cases: $fails assertion(s) FAILED"
exit 1
