#!/usr/bin/env bash
# ANTS-3794 — weekly local snapshot of the machine-global roadmap store.
#
# Usage: roadmap-store-backup.sh DEST_DIR [KEEP]
#
# Copies the store with sqlite3 .backup, which is safe while Ants holds a
# connection (the store runs in WAL; a plain cp is not). The copy must pass
# PRAGMA integrity_check before it is kept, then only the newest KEEP
# snapshots (default 8) survive.
#
# This is the stop-gap half of ANTS-3794. The durable half is the per-project
# JSONL export pushed to the private claude-config repo; a single-file
# snapshot cannot go there, because GitHub refuses files over 100 MB.
#
# A failed backup must not be silent (roadmap-data-model.md § 9): it exits
# non-zero, so a systemd timer marks the unit failed, and raises a desktop
# notification when notify-send is present.
set -euo pipefail

STORE="${XDG_DATA_HOME:-$HOME/.local/share}/ants-terminal/roadmap.sqlite"
DEST="${1:?usage: roadmap-store-backup.sh DEST_DIR [KEEP]}"
KEEP="${2:-8}"

fail() {
    echo "roadmap-store-backup: $*" >&2
    command -v notify-send >/dev/null &&
        notify-send -u critical "Roadmap store backup failed" "$*" || true
    exit 1
}

[[ "$KEEP" =~ ^[1-9][0-9]*$ ]] || fail "KEEP must be a positive integer, got '$KEEP'"
command -v sqlite3 >/dev/null || fail "sqlite3 is not installed"
[ -f "$STORE" ] || fail "no store at $STORE"
mkdir -p "$DEST" || fail "cannot create $DEST"

out="$DEST/roadmap-$(date +%Y%m%d-%H%M%S).sqlite"
tmp="$out.partial"
trap 'rm -f "$tmp"' EXIT

sqlite3 "$STORE" ".timeout 30000" ".backup '$tmp'" || fail "sqlite3 .backup to $tmp failed"
check="$(sqlite3 "$tmp" "PRAGMA integrity_check;" 2>&1)" || true
[ "$check" = "ok" ] || fail "integrity_check failed on the copy: $check"
chmod 600 "$tmp"
mv "$tmp" "$out"

# Keep the newest KEEP snapshots. Names sort by timestamp.
ls -1 "$DEST"/roadmap-*.sqlite 2>/dev/null | sort -r | tail -n +"$((KEEP + 1))" |
    while IFS= read -r old; do rm -f -- "$old"; done

echo "roadmap-store-backup: wrote $out"
