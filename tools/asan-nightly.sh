#!/usr/bin/env bash
# tools/asan-nightly.sh — keep build-asan warm overnight (user ruling
# 2026-10-10).
#
# The push gate's sanitizer leg runs only when build-asan is a few build
# steps behind (ANTS-4118's cost gate in tools/local-ci.sh); otherwise it is
# skipped and nothing sanitizes the push until GitHub's nightly run. Building
# the tree each night keeps that leg cheap enough to run.
#
# Builds only; the sanitized suite stays the push gate's. Shares
# build-asan/.ants-build.lock with that gate, because two ninjas in one tree
# corrupt it: whichever arrives second steps aside. Honours the gate's
# interrupted marker: a marked tree is rebuilt cold and the marker cleared,
# and a build this script loses to a signal leaves the marker behind.
#
#   tools/asan-nightly.sh            build now
#   tools/asan-nightly.sh --install  install a systemd --user timer (03:30)
#   tools/asan-nightly.sh --remove   remove that timer
#
# Exit: 0 built, skipped or (un)installed; 1 build failed; 143 interrupted.
set -uo pipefail
repo_root="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
asan_dir="${ANTS_ASAN_DIR:-$repo_root/build-asan}"
asan_marker="$asan_dir/.ants-prepush-interrupted"
unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
unit=ants-asan-nightly

case "${1:-}" in
    --install)
        mkdir -p "$unit_dir" || exit 1
        cat > "$unit_dir/$unit.service" <<EOF
[Unit]
Description=Ants Terminal: keep build-asan warm for the push gate

[Service]
Type=oneshot
ExecStart=$repo_root/tools/asan-nightly.sh
Nice=19
IOSchedulingClass=idle
EOF
        cat > "$unit_dir/$unit.timer" <<EOF
[Unit]
Description=Ants Terminal: nightly build-asan catch-up

[Timer]
OnCalendar=*-*-* 03:30
Persistent=false

[Install]
WantedBy=timers.target
EOF
        systemctl --user daemon-reload && systemctl --user enable --now "$unit.timer"
        exit $? ;;
    --remove)
        systemctl --user disable --now "$unit.timer" 2>/dev/null
        rm -f "$unit_dir/$unit.service" "$unit_dir/$unit.timer"
        systemctl --user daemon-reload
        exit 0 ;;
    "") ;;
    *) echo "asan-nightly: unknown argument '$1' (--install, --remove or nothing)" >&2
       exit 2 ;;
esac

if [[ ! -f "$asan_dir/CMakeCache.txt" ]]; then
    echo "asan-nightly: no build tree at $asan_dir — nothing to warm."
    exit 0
fi

exec 9>"$asan_dir/.ants-build.lock"
if ! flock -n 9; then
    echo "asan-nightly: $asan_dir is being built by something else — skipped."
    exit 0
fi

clean=()
if [[ -f "$asan_marker" ]]; then
    echo "asan-nightly: $asan_dir was interrupted mid-build — rebuilding it cold."
    clean=(--clean-first)
fi

# Started in the background and waited on: bash runs a trap only after a
# foreground command returns (the same reason as tools/qt62-guard.sh).
trap 'touch "$asan_marker"
      echo "asan-nightly: interrupted — $asan_dir marked untrusted." >&2
      exit 143' TERM INT HUP
cmake --build "$asan_dir" "${clean[@]}" &
if ! wait $!; then
    echo "asan-nightly: the build failed; the push gate will report it." >&2
    exit 1
fi
trap - TERM INT HUP
rm -f "$asan_marker"
echo "asan-nightly: $asan_dir is warm."
