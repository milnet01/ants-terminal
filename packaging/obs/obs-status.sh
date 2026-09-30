#!/bin/sh
# obs-status.sh (ANTS-3726) — wait until every repository has finished building
# the package's current source revision, then say how each one ended.
#
# Run after obs-submit.sh. Exit 0 = every repository built it. 1 = one failed,
# or (with --require-tests) built without running the test suite. 2 = still
# building when the polls ran out. 3 = the results could not be read.
#
#   --rev N           wait for source revision N instead of the current one
#   --require-tests   a repository only passes if its build log carries ctest's
#                     summary line, so a distro that skipped the suite is caught
#
# Override via env: OBS_API, OBS_PROJECT, OBS_PACKAGE, POLL_SECS (default 60),
# MAX_POLLS (default 120), STUCK_POLLS (default 10), TAIL_LINES (default 40).
#
# WHY IT READS JOB HISTORY. A repository's status word says what state it is
# in, not which revision that state is about. Right after a commit every
# repository still reads "succeeded" — for the PREVIOUS revision — so a waiter
# that matches status words can end before the new build has started. And the
# plain `osc results` text prints an unpublished success as `succeeded*`, which
# a word match never sees, so that waiter never ends at all. Both happened on
# 2026-09-30 (ANTS-5577). A job-history row is written once, when a build ends,
# and names the source revision it built: a row for this revision is the only
# thing that means "this build is over".
set -eu

API="${OBS_API:-https://api.opensuse.org}"
PROJ="${OBS_PROJECT:-home:milnet:ants-terminal}"
PKG="${OBS_PACKAGE:-ants-terminal}"
POLL_SECS="${POLL_SECS:-60}"
MAX_POLLS="${MAX_POLLS:-120}"
STUCK_POLLS="${STUCK_POLLS:-10}"
TAIL_LINES="${TAIL_LINES:-40}"

usage() {
    echo "obs-status: usage: obs-status.sh [--rev N] [--require-tests]" >&2
    exit 2
}

REV=""
REQUIRE_TESTS=0
while [ $# -gt 0 ]; do
    case "$1" in
        --rev)           [ $# -ge 2 ] || usage; REV="$2"; shift 2 ;;
        --require-tests) REQUIRE_TESTS=1; shift ;;
        *)               usage ;;
    esac
done

command -v osc >/dev/null 2>&1 || { echo "obs-status: osc not installed" >&2; exit 1; }

# One line per repository: "<repository> <arch> <status word>". Empty when the
# results cannot be read, which the caller treats as "not known yet".
results() {
    osc -A "$API" api "/build/$PROJ/_result?package=$PKG" 2>/dev/null | awk '
        /<result / {
            repo = ""; arch = ""
            if (match($0, / repository="[^"]*"/)) repo = substr($0, RSTART + 13, RLENGTH - 14)
            if (match($0, / arch="[^"]*"/))       arch = substr($0, RSTART + 7, RLENGTH - 8)
        }
        /<status / {
            if (match($0, / code="[^"]*"/)) print repo, arch, substr($0, RSTART + 7, RLENGTH - 8)
        }'
}

# How the newest finished build of revision >= REV ended in one repository, or
# nothing when no such build has finished. ">=" because a service re-run can
# add a revision on top of ours, and its build then stands in for ours.
job_code() {
    osc -A "$API" api "/build/$PROJ/$1/$2/_jobhistory?package=$PKG&limit=5" 2>/dev/null | awk -v want="$REV" '
        /<jobhist / {
            rev = ""; code = ""
            if (match($0, / rev="[0-9]+"/))  rev  = substr($0, RSTART + 6, RLENGTH - 7)
            if (match($0, / code="[^"]*"/)) code = substr($0, RSTART + 7, RLENGTH - 8)
            if (rev != "" && rev + 0 >= want + 0) last = code
        }
        END { if (last != "") print last }'
}

# The log of the last FINISHED build. nostream=1 makes the server return what
# it has instead of following a running build, which never ends (ANTS-5222);
# the time limit covers a stalled connection.
build_log() {
    timeout 300 osc -A "$API" api "/build/$PROJ/$1/$2/$PKG/_log?nostream=1&last=1" 2>/dev/null || true
}

if [ -z "$REV" ]; then
    REV="$(osc -A "$API" api "/source/$PROJ/$PKG" 2>/dev/null \
            | sed -n 's/^<directory[^>]* rev="\([0-9][0-9]*\)".*/\1/p' | head -n 1)"
fi
case "$REV" in
    ''|*[!0-9]*)
        echo "obs-status: could not read the source revision of $PROJ/$PKG from $API" >&2
        exit 3 ;;
esac
echo "obs-status: waiting for $PROJ/$PKG revision $REV"

i=0
stuck=0
res=""
while :; do
    i=$((i + 1))
    res="$(results)" || res=""
    pending=0
    sawstuck=0
    line=""
    [ -n "$res" ] || pending=1
    # A here-doc feeds the loop in this shell, so its variables survive it —
    # a pipeline would run the loop in a subshell and lose them.
    while read -r repo arch status; do
        [ -n "${status:-}" ] || continue
        case "$status" in excluded|disabled) continue ;; esac
        code="$(job_code "$repo" "$arch")"
        if [ -n "$code" ]; then
            line="$line $repo=$code"
            continue
        fi
        case "$status" in
            unresolvable|broken)
                # Either can be a passing state while the source service runs,
                # so it only counts as final once it has lasted STUCK_POLLS.
                sawstuck=1
                line="$line $repo=$status"
                [ "$stuck" -ge "$STUCK_POLLS" ] || pending=1 ;;
            *)
                pending=1
                line="$line $repo=waiting($status)" ;;
        esac
    done <<EOF
$res
EOF
    printf '[poll %s]%s\n' "$i" "${line:- results not readable}"
    if [ "$sawstuck" -eq 1 ]; then stuck=$((stuck + 1)); else stuck=0; fi
    [ "$pending" -eq 0 ] && break
    [ "$i" -ge "$MAX_POLLS" ] && break
    sleep "$POLL_SECS"
done

if [ -z "$res" ]; then
    echo "obs-status: could not read build results for $PROJ/$PKG from $API" >&2
    exit 3
fi

echo
echo "=== final (revision $REV) ==="
rc=0
while read -r repo arch status; do
    [ -n "${status:-}" ] || continue
    case "$status" in excluded|disabled) continue ;; esac
    code="$(job_code "$repo" "$arch")"
    case "$code" in
        succeeded|unchanged)
            if [ "$REQUIRE_TESTS" -eq 1 ]; then
                summary="$(build_log "$repo" "$arch" | grep -E '[0-9]+% tests passed' | tail -n 1)"
                if [ -n "$summary" ]; then
                    echo "$repo/$arch: $code — $summary"
                else
                    echo "$repo/$arch: $code, but its log has no ctest summary — the test suite did not run"
                    rc=1
                fi
            else
                echo "$repo/$arch: $code"
            fi ;;
        '')
            case "$status" in
                unresolvable|broken)
                    echo "$repo/$arch: $status — it cannot build. Details: osc -A $API results -v $PROJ $PKG"
                    rc=1 ;;
                *)
                    echo "$repo/$arch: no finished build of revision $REV after $i polls (status now: $status)"
                    if [ "$rc" -eq 0 ]; then rc=2; fi ;;
            esac ;;
        *)
            rc=1
            echo
            echo "=== $repo/$arch: $code — last $TAIL_LINES log lines ==="
            build_log "$repo" "$arch" | tail -n "$TAIL_LINES" ;;
    esac
done <<EOF
$res
EOF

exit "$rc"
