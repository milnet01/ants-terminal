#!/usr/bin/env bash
# ANTS-3794 — daily local snapshot of the machine-global roadmap store, and
# (ANTS-6311) of the know-how store where one exists: only a first entry
# creates that file, so its absence is not a failure.
#
# Usage: roadmap-store-backup.sh DEST_DIR [KEEP]
#
# Copies each store with sqlite3 .backup, which is safe while Ants holds a
# connection (the stores run in WAL; a plain cp is not). A copy must pass
# PRAGMA integrity_check before it is kept, then only the newest KEEP
# snapshots (default 8) of that store survive. A copy with under half the
# items of the largest kept snapshot fails the run and holds the full
# snapshots; ROADMAP_BACKUP_ACCEPT_SHRINK=1 accepts the new size (ANTS-5426).
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

# count_items FILE TABLE — the rows in TABLE; 0 where FILE has no such table.
count_items() {
    local n
    n=$(sqlite3 -readonly "$1" "select count(*) from $2;" 2>/dev/null) || n=0
    [[ "$n" =~ ^[0-9]+$ ]] || n=0
    printf '%s' "$n"
}

# ANTS-5426 — set by snapshot_store when a copy shrank; the run then fails
# after every store has been copied.
SHRUNK=""

# snapshot_store STORE NAME TABLE — copy STORE to DEST/NAME-<timestamp>.sqlite.
# TABLE holds the store's items, and its row count sizes a snapshot.
snapshot_store() {
    local store=$1 name=$2 table=$3 out check s n new largest=0 accepted hold=0
    local marker="$DEST/.$name-shrink-accepted"
    local -a snaps
    local -A count
    out="$DEST/$name-$(date +%Y%m%d-%H%M%S).sqlite"
    tmp="$out.partial"

    sqlite3 "$store" ".timeout 30000" ".backup '$tmp'" || fail "sqlite3 .backup to $tmp failed"
    check="$(sqlite3 "$tmp" "PRAGMA integrity_check;" 2>&1)" || true
    [ "$check" = "ok" ] || fail "integrity_check failed on the $name copy: $check"
    # Audit TL-7 — under set -e a bare failure here exited without fail(), so no
    # `snapshot fail` record and no notification.
    chmod 600 "$tmp" || fail "cannot chmod $tmp"
    mv "$tmp" "$out" || fail "cannot move the snapshot into place at $out"

    # ANTS-5426 — an emptied store passes integrity_check, and KEEP runs later
    # every good snapshot would be gone. A copy with under half the items of
    # the largest kept snapshot holds the full ones until the shrink is
    # accepted. The marker records the accepted size, so later plain runs pass
    # until the store halves again.
    snaps=("$DEST/$name"-*.sqlite)
    new=$(count_items "$out" "$table")
    for s in "${snaps[@]}"; do
        [ "$s" = "$out" ] && continue
        n=$(count_items "$s" "$table")
        count[$s]=$n
        (( n > largest )) && largest=$n
    done
    if (( new * 2 < largest )); then
        accepted=$(cat "$marker" 2>/dev/null) || accepted=""
        if [ "${ROADMAP_BACKUP_ACCEPT_SHRINK:-}" = 1 ]; then
            printf '%s\n' "$new" > "$marker" || fail "cannot write $marker"
        elif ! { [[ "$accepted" =~ ^[0-9]+$ ]] && (( new * 2 >= accepted )); }; then
            hold=1
            SHRUNK+="the $name copy holds $new item(s), under half of the $largest in a kept snapshot; full snapshots are held until a run with ROADMAP_BACKUP_ACCEPT_SHRINK=1. "
        fi
    else
        rm -f -- "$marker"
    fi

    # Keep the newest KEEP snapshots. The glob sorts ascending, and the names
    # carry a timestamp, so the oldest come first. A held snapshot has at
    # least twice the new copy's items.
    if (( ${#snaps[@]} > KEEP )); then
        for s in "${snaps[@]:0:${#snaps[@]}-KEEP}"; do
            (( hold && count[$s] > 0 && count[$s] >= new * 2 )) && continue
            rm -f -- "$s" || fail "cannot prune old snapshots in $DEST"
        done
    fi
    echo "roadmap-store-backup: wrote $out"
}

snapshot_store "$STORE" roadmap item

# ANTS-5247 — the canonical folder, read by roadmap_migrate's snapshot default.
# ANTS-5621 — and the store it backs up, so a migrate of another store skips it.
# Recorded before the know-how copy: its failure must report an error without
# hiding this success. A shrunken roadmap copy is not a success.
if [ -z "$SHRUNK" ]; then
    roadmap_backup_record snapshot ok "" "$(cd "$DEST" && pwd -P)" \
        "$(cd "$(dirname "$STORE")" && pwd -P)/$(basename "$STORE")"
fi

if [ -f "$KNOWHOW_STORE" ]; then
    snapshot_store "$KNOWHOW_STORE" knowhow know_how
fi
[ -z "$SHRUNK" ] || fail "$SHRUNK"
