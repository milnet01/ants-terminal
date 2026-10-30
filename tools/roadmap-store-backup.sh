#!/usr/bin/env bash
# ANTS-3794 — weekly local snapshot of the machine-global roadmap store, and
# (ANTS-6311) of the know-how store where one exists: only a first entry
# creates that file, so its absence is not a failure.
#
# Usage: roadmap-store-backup.sh DEST_DIR [KEEP]
#
# Copies each store with sqlite3 .backup, which is safe while Ants holds a
# connection (the stores run in WAL; a plain cp is not). A copy must pass
# PRAGMA integrity_check before it is kept, then only the newest KEEP
# snapshots (default 8) of that store survive.
#
# This is the stop-gap half of ANTS-3794. The durable half is the per-project
# JSONL export pushed to the private claude-config repo; a single-file
# snapshot cannot go there, because GitHub refuses files over 100 MB.
#
# A failed backup must not be silent (roadmap-data-model.md § 9): it exits
# non-zero, so a systemd timer marks the unit failed, and raises a desktop
# notification when notify-send is present. Every run writes the `snapshot`
# backup record (ANTS-3794 § 2.4), which session_orient reads to report a
# backup that has stopped (§ 2.5).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=tools/roadmap-backup-lib.sh
. "$here/roadmap-backup-lib.sh"

DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/ants-terminal"
STORE="$DATA_DIR/roadmap.sqlite"
KNOWHOW_STORE="$DATA_DIR/knowhow.sqlite"
DEST="${1:?usage: roadmap-store-backup.sh DEST_DIR [KEEP]}"
KEEP="${2:-8}"

fail() {
    echo "roadmap-store-backup: $*" >&2
    roadmap_backup_record snapshot fail "$*" || true
    roadmap_backup_notify "$*"
    exit 1
}

[[ "$KEEP" =~ ^[1-9][0-9]*$ ]] || fail "KEEP must be a positive integer, got '$KEEP'"
command -v sqlite3 >/dev/null || fail "sqlite3 is not installed"
[ -f "$STORE" ] || fail "no store at $STORE"
# Audit TL-9 — the path goes inside a quoted sqlite3 dot-command below.
case "$DEST" in *"'"*) fail "DEST must not contain a single quote: $DEST" ;; esac
mkdir -p "$DEST" || fail "cannot create $DEST"
# Audit TL-9 — two runs in one second shared one .partial name, and one run's
# EXIT trap deleted the other's copy. Serialise runs on a lock in DEST.
exec 9>"$DEST/.roadmap-store-backup.lock" || fail "cannot open the lock in $DEST"
flock -w 60 9 || fail "another backup into $DEST is still running"

tmp=""
trap 'rm -f "$tmp"' EXIT
shopt -s nullglob

# snapshot_store STORE NAME — copy STORE to DEST/NAME-<timestamp>.sqlite.
snapshot_store() {
    local store=$1 name=$2 out check
    local -a snaps
    out="$DEST/$name-$(date +%Y%m%d-%H%M%S).sqlite"
    tmp="$out.partial"

    sqlite3 "$store" ".timeout 30000" ".backup '$tmp'" || fail "sqlite3 .backup to $tmp failed"
    check="$(sqlite3 "$tmp" "PRAGMA integrity_check;" 2>&1)" || true
    [ "$check" = "ok" ] || fail "integrity_check failed on the $name copy: $check"
    # Audit TL-7 — under set -e a bare failure here exited without fail(), so no
    # `snapshot fail` record and no notification.
    chmod 600 "$tmp" || fail "cannot chmod $tmp"
    mv "$tmp" "$out" || fail "cannot move the snapshot into place at $out"

    # Keep the newest KEEP snapshots. The glob sorts ascending, and the names
    # carry a timestamp, so the oldest come first.
    snaps=("$DEST/$name"-*.sqlite)
    if (( ${#snaps[@]} > KEEP )); then
        rm -f -- "${snaps[@]:0:${#snaps[@]}-KEEP}" || fail "cannot prune old snapshots in $DEST"
    fi
    echo "roadmap-store-backup: wrote $out"
}

snapshot_store "$STORE" roadmap

# ANTS-5247 — the canonical folder, read by roadmap_migrate's snapshot default.
# ANTS-5621 — and the store it backs up, so a migrate of another store skips it.
# Recorded before the know-how copy: its failure must report an error without
# hiding this success.
roadmap_backup_record snapshot ok "" "$(cd "$DEST" && pwd -P)" \
    "$(cd "$(dirname "$STORE")" && pwd -P)/$(basename "$STORE")"

if [ -f "$KNOWHOW_STORE" ]; then
    snapshot_store "$KNOWHOW_STORE" knowhow
fi
