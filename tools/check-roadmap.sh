#!/usr/bin/env bash
# tools/check-roadmap.sh — pre-commit / CI guard for ROADMAP.md.
# Fails with non-zero exit when two or more bullets share the same
# [PROJ-NNNN] id, surfacing the hand-edit drift the `.roadmap-counter`
# guard `roadmap_log` op:append would have caught.
#
# ANTS-1646 — pull 32, 2026-05-19.
#
# Usage:
#   tools/check-roadmap.sh                # scan ROADMAP.md at repo root
#   tools/check-roadmap.sh path/to/RM.md  # scan a specific file
#   tools/check-roadmap.sh --allow ID,ID  # exempt known intentional cites
#
# The `--allow` list takes comma-separated bare IDs (no brackets, no
# `ANTS-` prefix expected — pass them verbatim). The current
# allowlist is ANTS-1118 — same shipped fix cross-cited in a tier
# table; remove from the allowlist once that section is rewritten.
#
# Exit codes:
#   0  clean (or every duplicate exempt via --allow)
#   1  one or more duplicate IDs found
#   2  setup error (file missing, bad args)

set -euo pipefail

usage() {
    cat <<'EOF'
tools/check-roadmap.sh — fail when ROADMAP.md has duplicate bullet IDs.

Usage:
  tools/check-roadmap.sh [path]            scan the file (default ROADMAP.md)
  tools/check-roadmap.sh [path] --allow ID,ID
                                           exempt the listed bare IDs

Exit 0 = clean; 1 = duplicate found; 2 = setup error.
EOF
}

target="ROADMAP.md"
allow_csv="ANTS-1118"   # known intentional cross-section cite

while [ "$#" -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --allow)   allow_csv="${2:-}"; shift 2 ;;
        --) shift; break ;;
        -*) printf 'check-roadmap: unknown flag %s\n' "$1" >&2
            usage >&2; exit 2 ;;
        *)  target="$1"; shift ;;
    esac
done

if [ ! -f "$target" ]; then
    printf 'check-roadmap: %s not found\n' "$target" >&2
    exit 2
fi

# Build a regex of allowed ids so the awk pass can skip them. The
# CSV may be empty (caller passed --allow "").
allow_regex=""
if [ -n "$allow_csv" ]; then
    # Escape any regex metachars in the ids; in practice they're
    # bare PROJ-NNNN tokens but we don't want a stray dot to bite.
    allow_regex=$(printf '%s' "$allow_csv" | sed 's/[]\.[^$*]/\\&/g; s/,/|/g')
fi

# Extract bullet-position ids (the leading `- <status-emoji> [ID]`
# pattern matches both ants-v1 bullets and the synthetic GFM
# adapter's emitted bullets). Tally and print collisions.
dupes=$(
    awk -v allow="$allow_regex" '
        # Bullet start: `- ` (any non-space char takes the status
        # role — emoji width is 4 bytes UTF-8 so byte indices vary).
        # Match "[ID]" after the leading "- " token.
        /^- [^ ]/ {
            n = match($0, /\[[A-Za-z][A-Za-z0-9_-]*-[0-9]+\]/)
            if (n > 0) {
                id = substr($0, n, RLENGTH)
                count[id]++
                if (count[id] == 2) order[++seen] = id
            }
        }
        END {
            for (i = 1; i <= seen; i++) {
                id = order[i]
                bare = id; gsub(/^\[|\]$/, "", bare)
                if (allow != "" && match(bare, "^(" allow ")$")) continue
                printf "%s (%d occurrences)\n", id, count[id]
            }
        }
    ' "$target"
)

if [ -n "$dupes" ]; then
    printf 'check-roadmap: duplicate bullet ids in %s:\n' "$target" >&2
    # awk emits one finding per line; preserve that shape verbatim.
    printf '%s\n' "$dupes" | sed 's/^/  /' >&2
    printf 'check-roadmap: rename via `roadmap_log op:append` or pass --allow if intentional.\n' >&2
    exit 1
fi

printf 'check-roadmap: %s clean.\n' "$target"
exit 0
