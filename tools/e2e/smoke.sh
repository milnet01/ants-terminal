#!/usr/bin/env bash
# ANTS-2049 — e2e smoke suite (spec §6). Sources run.sh, drives a throwaway
# --e2e instance, and asserts each harness capability end-to-end. Exit 0 iff
# every case passes; a SKIP (e.g. no visible dialog under offscreen) is not a
# failure. Run by hand or via `ctest -L e2e`. NO build step (INV-7).
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
# shellcheck source=tools/e2e/run.sh
source tools/e2e/run.sh

fails=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; fails=$((fails + 1)); }
skip() { echo "SKIP  $*"; }
has()  { grep -q "$2" <<<"$1"; }

launch_e2e main || { echo "FATAL: could not launch --e2e instance"; exit 1; }

# ── Case 1: terminal echo — send-text drives the PTY, get-text observes ──
call_e2e '{"cmd":"send-text","text":"echo ANTS_E2E_OK\n"}' >/dev/null
sleep 1
if has "$(call_e2e '{"cmd":"get-text"}')" 'ANTS_E2E_OK'; then
    pass "1 terminal echo (send-text → get-text)"
else
    fail "1 terminal echo — sentinel not in get-text"
fi

# ── Case 2: GUI key event — inject-key a sentinel into the focused grid ──
call_e2e '{"cmd":"inject-key","text":"ANTS_E2E_KEY"}' >/dev/null
sleep 1
if has "$(call_e2e '{"cmd":"get-text"}')" 'ANTS_E2E_KEY'; then
    pass "2 inject-key sentinel reached the grid"
elif [[ "${QT_QPA_PLATFORM:-}" == offscreen ]]; then
    skip "2 inject-key (offscreen: grid keyboard focus not guaranteed)"
else
    fail "2 inject-key — sentinel not observed in get-text"
fi

# ── Case 3: dialog open + grab (skip-not-fail if no visible dialog) ──
call_e2e '{"cmd":"inject-click","widget":"roadmapButton"}' >/dev/null
dlg=no
for _ in $(seq 1 12); do   # ~3 s
    if has "$(call_e2e '{"cmd":"grab-image","widget":"RoadmapDialog","path":"'"$(e2e_art)"'/dlg.png"}')" '"ok":true'; then
        dlg=yes; break
    fi
    sleep 0.25
done
if [[ "$dlg" == yes && -s "$(e2e_art)/dlg.png" ]]; then
    pass "3 roadmap dialog opened + grabbed (PNG non-zero)"
else
    skip "3 dialog open+grab (no visible RoadmapDialog — expected under offscreen)"
fi

# ── Case 4: window resize echoes the applied size ──
r=$(call_e2e '{"cmd":"resize-window","w":1000,"h":700}')
if has "$r" '"ok":true' && has "$r" '"h":700'; then
    pass "4 resize-window echoes {w,h}"
else
    fail "4 resize-window — reply=$r"
fi

# ── Case 5: gate negative — a plain (non-e2e) instance refuses inject ──
launch_e2e gate --no-e2e || fail "5 gate instance launch"
g=$(call_e2e '{"cmd":"inject-key","text":"x"}' gate)
if has "$g" 'e2e_disabled'; then
    pass "5 gate: non-e2e instance refuses inject-key (e2e_disabled)"
else
    fail "5 gate — reply=$g"
fi

# ── Case 6: bad widget → widget_not_found, app stays alive ──
b=$(call_e2e '{"cmd":"inject-click","widget":"nope-xyz"}')
if has "$b" 'widget_not_found' && has "$(call_e2e '{"cmd":"tab-list"}')" '"ok":true'; then
    pass "6 bad widget → widget_not_found, app alive"
else
    fail "6 bad widget — reply=$b"
fi

# ── Case 7: grab guards — escape → bad_path; no artifact dir → bad_args ──
esc=$(call_e2e '{"cmd":"grab-image","path":"/etc/x.png"}')
launch_e2e noart --no-artifact-dir
noart=$(call_e2e '{"cmd":"grab-image","path":"whatever.png"}' noart)
if has "$esc" 'bad_path' && has "$noart" 'bad_args'; then
    pass "7 grab guards: escape → bad_path, unset dir → bad_args"
else
    fail "7 grab guards — esc=$esc noart=$noart"
fi

# ── Case 8: teardown reaps instances + temp dirs (INV-6) ──
d_main=$(dirname "$(e2e_art main)")
d_gate=$(dirname "$(e2e_art gate)")
d_noart=$(dirname "$(e2e_art noart)")
teardown_e2e   # the EXIT trap will also run harmlessly at script exit
if [[ ! -e "$d_main" && ! -e "$d_gate" && ! -e "$d_noart" ]]; then
    pass "8 teardown reaped every temp dir"
else
    fail "8 teardown — a temp dir survived"
fi

echo
if [[ "$fails" == 0 ]]; then
    echo "e2e smoke: ALL PASS (skips allowed)"
    exit 0
fi
echo "e2e smoke: $fails case(s) FAILED"
exit 1
