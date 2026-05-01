#!/usr/bin/env bash
#
# packaging/rotate-roadmap.sh — ANTS-1125 rotation step.
#
# Snip the closed minor's section out of ROADMAP.md and write it to
# docs/roadmap/<closed-minor>.md (creating the dir if needed). Invoked
# by /bump on minor or major version bumps; not on patch bumps. See
# tests/features/roadmap_viewer_archive/spec.md § INV-14 for the
# contract this script implements.
#
# Usage:
#   packaging/rotate-roadmap.sh <CLOSED-MINOR> [<roadmap-path>] [<archive-dir>]
#
#   CLOSED-MINOR    e.g. "0.7" — the minor whose section should be
#                   rotated out. The script matches the next "## 0.7"
#                   heading (any `0.7.NN — …` form) and walks until
#                   the next top-level "## " heading that does NOT
#                   begin with "## 0.7".
#   roadmap-path    defaults to ./ROADMAP.md
#   archive-dir     defaults to ./docs/roadmap
#
# Exit codes:
#   0 — rotated cleanly OR nothing to rotate (idempotent).
#   1 — bad usage / can't read source / can't write archive.
#   2 — closed minor's section couldn't be located unambiguously.

set -euo pipefail

CLOSED="${1:-}"
ROADMAP="${2:-ROADMAP.md}"
ARCHIVE_DIR="${3:-docs/roadmap}"

if [[ -z "$CLOSED" ]]; then
    echo "usage: rotate-roadmap.sh <closed-minor> [roadmap-path] [archive-dir]" >&2
    exit 1
fi

if [[ ! -f "$ROADMAP" ]]; then
    echo "rotate-roadmap.sh: $ROADMAP not found" >&2
    exit 1
fi

# Validate the minor format — must be ^[0-9]+\.[0-9]+$ to match
# INV-4a's archive-naming filter.
if ! [[ "$CLOSED" =~ ^[0-9]+\.[0-9]+$ ]]; then
    echo "rotate-roadmap.sh: closed-minor '$CLOSED' must be MAJOR.MINOR" >&2
    exit 1
fi

ARCHIVE_FILE="$ARCHIVE_DIR/$CLOSED.md"
mkdir -p "$ARCHIVE_DIR"

# Cold-eyes-review H3 (2026-04-30): refuse to clobber an existing
# archive. If the archive already exists, the previous rotation
# already ran successfully — re-running should be a no-op for the
# archive write. We still proceed to snip from ROADMAP.md if a
# matching section is still there (edge case: someone manually
# re-pasted the closed minor's section back into ROADMAP.md after a
# rotation — they want the snip; they don't want their archive
# overwritten).
ARCHIVE_EXISTED=0
if [[ -e "$ARCHIVE_FILE" ]]; then
    ARCHIVE_EXISTED=1
fi

# Find the first line whose text starts with "## $CLOSED." or "## $CLOSED ".
# (Handles "## 0.7.0 — …" and "## 0.7 archive" forms.) Bail with exit 0
# if no such heading exists — idempotent on a roadmap that's already
# been rotated. Cold-eyes-review H1 (2026-04-30): the literal `.` chars
# in `$CLOSED` must be regex-escaped so `## 0.7` doesn't accidentally
# match `## 0X7` or `## 027` should the version ever stretch wider.
ESCAPED=$(printf '%s' "$CLOSED" | sed 's/\./\\\\./g')
START_LINE=$(awk -v pat="^## ${ESCAPED}[. ]" '
    $0 ~ pat { print NR; exit }
' "$ROADMAP")

if [[ -z "$START_LINE" ]]; then
    echo "rotate-roadmap.sh: no '## $CLOSED.' section found in $ROADMAP — nothing to rotate" >&2
    exit 0
fi

# Walk forward to the next "## " heading that does NOT begin with the
# closed-minor prefix. That's the end of the rotated block (exclusive).
END_LINE=$(awk -v start="$START_LINE" -v pat="^## ${ESCAPED}[. ]" '
    NR > start && /^## / && $0 !~ pat { print NR; exit }
' "$ROADMAP")

# If the section runs to EOF, treat the file end as the end marker.
if [[ -z "$END_LINE" ]]; then
    END_LINE=$(($(wc -l < "$ROADMAP") + 1))
fi

# Sanity guard: end must be strictly after start.
if (( END_LINE <= START_LINE )); then
    echo "rotate-roadmap.sh: degenerate section bounds [$START_LINE, $END_LINE)" >&2
    exit 2
fi

# Write the archive file. The header explains where it came from so a
# reader who arrives at docs/roadmap/0.7.md without context can trace
# the move. The body is the snipped content verbatim.
#
# Cold-eyes-review H3/M2/M3 (2026-04-30): if the archive already
# existed before this run, leave it alone (no-clobber) — the rotation
# is still useful for snipping ROADMAP.md, but the existing archive
# is the source of truth. Otherwise, write atomically (mktemp + mv)
# so a crash mid-write doesn't leave a half-written archive.
if [[ $ARCHIVE_EXISTED -eq 1 ]]; then
    echo "rotate-roadmap.sh: $ARCHIVE_FILE already exists — preserving it (no-clobber)" >&2
else
    ARCHIVE_TMP="$(mktemp "${ARCHIVE_FILE}.write.XXXXXX")"
    {
        echo "# $CLOSED.x archive"
        echo
        echo "This file is the post-$CLOSED archive — \`/bump\` rotated"
        echo "the closed $CLOSED.x sections out of \`ROADMAP.md\` here so"
        echo "the working roadmap stays small. See"
        echo "[\`docs/standards/roadmap-format.md\` § 3.9 Archive rotation]"
        echo "(../standards/roadmap-format.md) for the rotation contract."
        echo
        sed -n "${START_LINE},$((END_LINE - 1))p" "$ROADMAP"
    } > "$ARCHIVE_TMP"
    mv "$ARCHIVE_TMP" "$ARCHIVE_FILE"
fi

# Snip the section out of ROADMAP.md atomically. Write to a temp,
# rename over the original — same atomic-replace pattern Ants uses
# elsewhere (cf. SessionManager std::rename, ANTS-1016).
TMP="$(mktemp "${ROADMAP}.rotate.XXXXXX")"
awk -v start="$START_LINE" -v end="$END_LINE" '
    NR < start || NR >= end { print }
' "$ROADMAP" > "$TMP"
mv "$TMP" "$ROADMAP"

echo "rotate-roadmap.sh: rotated $CLOSED.x → $ARCHIVE_FILE (${START_LINE}..$((END_LINE - 1)))"
