#!/usr/bin/env bash
# ANTS-3794 § 2.3 — export every roadmap in the store and publish the result
# to a clone of the private claude-config repo.
#
# Usage: roadmap-export-publish.sh <repo>
#
# Writes <repo>/roadmap-export/<export_slug>.jsonl through
# `ants-terminal --export-roadmaps`, commits only those files, and pushes.
# It never merges or rebases: when the upstream has a commit HEAD lacks, two
# stores (or two writers) have diverged, and that must surface rather than be
# folded in (roadmap-data-model.md § 9).
#
# Exit codes: 0 done (including "nothing to commit"), 1 failed, 3 another run
# holds the lock. Every failure writes the `export` backup record (§ 2.4) and
# raises a desktop notification; the lock-held exit does neither.
#
# The binary defaults to the home copy launch.sh keeps up to date; override
# with ANTS_ROADMAP_EXPORT_BIN.
set -uo pipefail

here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=tools/roadmap-backup-lib.sh
. "$here/roadmap-backup-lib.sh"

REPO="${1:?usage: roadmap-export-publish.sh <repo>}"
BIN="${ANTS_ROADMAP_EXPORT_BIN:-${XDG_DATA_HOME:-$HOME/.local/share}/ants-terminal/bin/ants-terminal}"
STATE=$(roadmap_backup_state_dir)
PATHSPEC='roadmap-export/*.jsonl'

fail() {
    echo "roadmap-export-publish: $*" >&2
    roadmap_backup_record export fail "$*"
    roadmap_backup_notify "$*"
    exit 1
}

# Step 1 — one run at a time. A held lock is not a failure: no record, no
# notification, no repository touched.
mkdir -p "$STATE" || fail "cannot create $STATE"
command -v flock >/dev/null || fail "flock is not installed"
exec 9>"$STATE/roadmap-backup-export.lock"
if ! flock -n 9; then
    echo "roadmap-export-publish: another run is in progress" >&2
    exit 3
fi

# Step 2 — a work tree on a branch with an upstream, and nothing in progress.
command -v git >/dev/null || fail "git is not installed"
git -C "$REPO" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    fail "$REPO is not a git work tree"
git -C "$REPO" symbolic-ref -q HEAD >/dev/null || fail "$REPO has a detached HEAD"
upstream=$(git -C "$REPO" rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null) ||
    fail "$REPO's current branch has no upstream"
gitdir=$(git -C "$REPO" rev-parse --absolute-git-dir) || fail "cannot find $REPO's git dir"
for marker in MERGE_HEAD rebase-merge rebase-apply CHERRY_PICK_HEAD; do
    [ -e "$gitdir/$marker" ] && fail "$REPO has a $marker in progress"
done

# Step 3 — refuse to publish over a diverged upstream.
git -C "$REPO" fetch --quiet "${upstream%%/*}" || fail "git fetch ${upstream%%/*} failed"
behind=$(git -C "$REPO" rev-list --abbrev-commit HEAD.."$upstream" | tr '\n' ' ')
if [ -n "$behind" ]; then
    touched=$(git -C "$REPO" rev-list --abbrev-commit HEAD.."$upstream" -- roadmap-export/ | tr '\n' ' ')
    fail "upstream $upstream has commits HEAD lacks (${behind% }); touching roadmap-export/: ${touched:-none}. Not merging."
fi

# Step 4 — export.
[ -x "$BIN" ] || fail "no ants-terminal binary at $BIN"
out=$("$BIN" --export-roadmaps "$REPO/roadmap-export" 2>&1)
rc=$?
printf '%s\n' "$out"
[ "$rc" -eq 0 ] || fail "export exited $rc: $(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"

# Steps 5-6 — commit the export files only, then push. Hooks run.
git -C "$REPO" add -A -- "$PATHSPEC" || fail "git add $PATHSPEC failed"
if git -C "$REPO" diff --cached --quiet -- "$PATHSPEC"; then
    echo "roadmap-export-publish: nothing to commit"
else
    git -C "$REPO" commit -q -m "chore: weekly roadmap export ($(date -u +%F))" -- "$PATHSPEC" ||
        fail "git commit failed (a hook may have refused it)"
    git -C "$REPO" push --quiet || fail "git push to $upstream failed"
    echo "roadmap-export-publish: committed and pushed $(git -C "$REPO" rev-parse --short HEAD)"
fi

# Step 7.
roadmap_backup_record export ok
