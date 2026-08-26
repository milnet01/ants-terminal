#!/usr/bin/env bash
# ANTS-4714 — the converse of cut-release Phase 0f.
#
# Phase 0f asks: every id the CHANGELOG claims, is it shipped? This asks the
# other direction: every item the roadmap store says shipped in the release
# window, is it in the CHANGELOG? A release whose notes silently omit shipped
# work is the same class of defect as one that overclaims, and nothing checked
# it until now.
#
# The store is the only source that can answer it. `roadmap_query mode:"report"`
# returns COUNTS over a window and no ids, so the missing set is not derivable
# from the verb — hence the direct read. Read-only, and safe while Ants holds
# its own connection (the store runs in WAL).
#
# Exit 0 = every shipped item in the window is recorded, or the check could not
# run and said so. Exit 1 = uncovered items, listed.
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$ROOT" || exit 0
DB="${XDG_DATA_HOME:-$HOME/.local/share}/ants-terminal/roadmap.sqlite"
CHANGELOG="$ROOT/CHANGELOG.md"
SINCE=""
QUIET=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --since) SINCE="${2:-}"; shift 2 ;;
        --quiet) QUIET=1; shift ;;
        -h|--help)
            echo "usage: check-shipped-coverage.sh [--since YYYY-MM-DD] [--quiet]"
            echo "  Reports roadmap items shipped since DATE that no CHANGELOG"
            echo "  bullet cites. Default DATE is the latest public release tag."
            exit 0 ;;
        *) echo "check-shipped-coverage: unknown argument: $1" >&2; exit 2 ;;
    esac
done

# --- Preconditions. A gate whose input is absent SKIPs LOUDLY; it never
#     reports a clean pass it did not earn.
if ! command -v sqlite3 >/dev/null 2>&1; then
    echo "shipped-coverage: ⊘ SKIPPED — sqlite3 not on PATH, so the store"
    echo "                  cannot be read. Install sqlite3 to enable this gate."
    exit 0
fi
if [[ ! -r "$DB" ]]; then
    echo "shipped-coverage: ⊘ SKIPPED — no roadmap store at $DB."
    echo "                  A markdown-backed project has no shipped dates to check."
    exit 0
fi
if [[ ! -r "$CHANGELOG" ]]; then
    echo "shipped-coverage: ⊘ SKIPPED — no CHANGELOG.md to check against."
    exit 0
fi

q() { sqlite3 "file:${DB}?mode=ro" "$1" 2>/dev/null; }

PROJECT_ID="$(q "SELECT project_id FROM project WHERE root='${ROOT}' LIMIT 1;")"
if [[ -z "$PROJECT_ID" ]]; then
    echo "shipped-coverage: ⊘ SKIPPED — this project is not registered in the"
    echo "                  store, so it has no shipped dates. Run roadmap_migrate"
    echo "                  first if you want this gate to cover it."
    exit 0
fi

# --- Window. Default to the newest public tag's date, so the window is
#     "since the last thing users received" rather than an arbitrary span.
if [[ -z "$SINCE" ]]; then
    LAST_TAG="$(git tag --list 'v[0-9]*' --sort=-v:refname \
                 | grep -vE -- '-rc' | head -1)"
    if [[ -n "$LAST_TAG" ]]; then
        SINCE="$(git log -1 --format=%ad --date=short "$LAST_TAG" 2>/dev/null)"
    fi
fi
if [[ -z "$SINCE" ]]; then
    echo "shipped-coverage: ⊘ SKIPPED — no public release tag found and no"
    echo "                  --since given, so there is no window to check."
    exit 0
fi

# --- What the store says shipped in the window.
mapfile -t SHIPPED < <(q "
    SELECT id FROM item
    WHERE project_id=${PROJECT_ID}
      AND shipped IS NOT NULL AND shipped >= '${SINCE}'
    ORDER BY id;")

# --- Honesty about what the dates cannot see. The store stamps `shipped`
#     going forward from 2026-08-20; rows closed before that are NULL unless
#     roadmap_log op:"backfill_dates" has been run. An undated row is invisible
#     to this gate, so say how many there are rather than implying coverage.
UNDATED="$(q "SELECT COUNT(*) FROM item
             WHERE project_id=${PROJECT_ID}
               AND status='shipped' AND shipped IS NULL;")"

if [[ ${#SHIPPED[@]} -eq 0 ]]; then
    echo "shipped-coverage: no dated ships since ${SINCE} — nothing to check."
    [[ "${UNDATED:-0}" -gt 0 ]] && echo "                  (${UNDATED} shipped items carry no date and are not covered;"
    [[ "${UNDATED:-0}" -gt 0 ]] && echo "                   run roadmap_log op:\"backfill_dates\" to date them.)"
    exit 0
fi

# --- Which of them a CHANGELOG bullet cites. Position matters: an id in
#     continuation prose is a cross-reference, not a claim that it shipped, so
#     only a line STARTING a bullet counts — the same rule cut-release Phase 0f
#     applies in the other direction.
UNCOVERED=()
for ID in "${SHIPPED[@]}"; do
    [[ -z "$ID" ]] && continue
    if ! grep -qE "^[[:space:]]*[-*][[:space:]].*\(${ID}\)" "$CHANGELOG"; then
        UNCOVERED+=("$ID")
    fi
done

if [[ ${#UNCOVERED[@]} -eq 0 ]]; then
    [[ $QUIET -eq 1 ]] || echo "shipped-coverage: all ${#SHIPPED[@]} items shipped since ${SINCE} are recorded."
    [[ "${UNDATED:-0}" -gt 0 && $QUIET -eq 0 ]] && \
        echo "                  (${UNDATED} older shipped items carry no date and were not checked.)"
    exit 0
fi

echo "shipped-coverage: ${#UNCOVERED[@]} of ${#SHIPPED[@]} items shipped since ${SINCE}"
echo "                  are cited by no CHANGELOG bullet:"
echo
for ID in "${UNCOVERED[@]}"; do
    LINE="$(q "SELECT kind || ' — ' || headline FROM item
               WHERE project_id=${PROJECT_ID} AND id='${ID}' LIMIT 1;")"
    printf '  %-12s %s\n' "$ID" "${LINE:0:96}"
done
echo
echo "Each is either release-note-worthy — add it with changelog_log — or"
echo "deliberately internal, in which case say so in the release report rather"
echo "than leaving it to look like an oversight."
[[ "${UNDATED:-0}" -gt 0 ]] && \
    echo "(${UNDATED} older shipped items carry no date and were not checked.)"
exit 1
