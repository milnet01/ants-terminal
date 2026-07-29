#!/bin/sh
# obs-status.sh (ANTS-3726) — poll OBS build results until every repository
# reaches a terminal state, then print the tail of any log that did not succeed.
#
# Run standalone after obs-submit.sh. Override via env: OBS_API, OBS_PROJECT,
# OBS_PACKAGE, POLL_SECS (default 60), MAX_POLLS (default 60).
set -eu

API="${OBS_API:-https://api.opensuse.org}"
PROJ="${OBS_PROJECT:-home:milnet:ants-terminal}"
PKG="${OBS_PACKAGE:-ants-terminal}"
POLL_SECS="${POLL_SECS:-60}"
MAX_POLLS="${MAX_POLLS:-60}"
TAIL_LINES="${TAIL_LINES:-40}"

command -v osc >/dev/null 2>&1 || { echo "obs-status: osc not installed" >&2; exit 1; }

# Terminal states. Anything else (scheduled/building/dispatching/blocked/
# signing/finished/unknown) means keep waiting.
is_terminal() {
    case "$1" in
        succeeded|failed|unresolvable|broken|excluded|disabled) return 0 ;;
        *) return 1 ;;
    esac
}

i=0
while [ "$i" -lt "$MAX_POLLS" ]; do
    i=$((i + 1))
    res="$(osc -A "$API" results "$PROJ" "$PKG" 2>/dev/null || true)"
    printf '[poll %s] %s\n' "$i" "$(echo "$res" | awk 'NF{printf "%s/%s=%s ", $1, $2, $4}')"

    pending=0
    # A here-doc feeds the loop in this shell, so `pending` survives it —
    # a pipeline would run the loop in a subshell and the assignment would be lost.
    while read -r _repo _arch _p status _rest; do
        [ -n "${status:-}" ] || continue
        is_terminal "$status" || pending=1
    done <<EOF
$res
EOF

    [ "$pending" -eq 0 ] && break
    sleep "$POLL_SECS"
done

echo
echo "=== final ==="
osc -A "$API" results "$PROJ" "$PKG" || true

# Print the tail of every non-succeeded repo's log, so a failure is diagnosable
# without a second command.
rc=0
while read -r repo arch _p status _rest; do
    [ -n "${status:-}" ] || continue
    case "$status" in
        succeeded|excluded|disabled) continue ;;
    esac
    rc=1
    echo
    echo "=== $repo/$arch: $status — last $TAIL_LINES log lines ==="
    osc -A "$API" buildlog "$PROJ" "$PKG" "$repo" "$arch" 2>/dev/null | tail -n "$TAIL_LINES" || true
done <<EOF
$(osc -A "$API" results "$PROJ" "$PKG" 2>/dev/null || true)
EOF

exit "$rc"
