# shellcheck shell=bash
# ANTS-3794 § 2.4 — the backup record, shared by tools/roadmap-store-backup.sh
# (job "snapshot") and tools/roadmap-export-publish.sh (job "export").
# Sourced, not run. RoadmapBackupHealth::assess() reads what this writes, so
# the format lives in one place on each side.
#
# The record is three key=value lines, always all three:
#   attempt=<UTC ISO-8601>   every run that got past the lock
#   success=<UTC ISO-8601>   changed only on success; empty until the first
#   error=<one line>         emptied on success, set on failure
#   dest=<folder>            snapshot job only (ANTS-5247): the canonical
#                            DEST_DIR of the last success; empty until then,
#                            carried across a failure like success

roadmap_backup_state_dir() {
    printf '%s/ants-terminal' "${XDG_STATE_HOME:-$HOME/.local/state}"
}

# roadmap_backup_record <job> ok|fail [error text] [dest folder]
roadmap_backup_record() {
    local job=$1 outcome=$2 err=${3:-} newdest=${4:-}
    local dir file now success="" dest=""
    dir=$(roadmap_backup_state_dir)
    file="$dir/roadmap-backup-$job.state"
    mkdir -p "$dir" || return 1
    now=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    [ -f "$file" ] && success=$(sed -n 's/^success=//p' "$file" | head -n 1)
    [ -f "$file" ] && dest=$(sed -n 's/^dest=//p' "$file" | head -n 1)
    if [ "$outcome" = ok ]; then
        success=$now
        err=""
        [ -n "$newdest" ] && dest=$newdest
    else
        err=$(printf '%s' "${err:-unknown failure}" | tr '\n\r' '  ')
    fi
    {
        printf 'attempt=%s\nsuccess=%s\nerror=%s\n' "$now" "$success" "$err"
        if [ "$job" = snapshot ]; then printf 'dest=%s\n' "$dest"; fi
    } > "$file.tmp" && mv "$file.tmp" "$file"
}

# roadmap_backup_notify <message> — a desktop notification when one is possible.
roadmap_backup_notify() {
    command -v notify-send >/dev/null 2>&1 &&
        notify-send -u critical "Roadmap store backup failed" "$1" || true
}
