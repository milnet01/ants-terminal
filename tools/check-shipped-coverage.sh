#!/usr/bin/env bash
# ANTS-4714 — the converse of cut-release Phase 0f.
#
# Phase 0f asks: every id the CHANGELOG claims, is it shipped? This asks the
# other direction: every item the roadmap store says shipped in the release
# window, is it in the CHANGELOG? A release whose notes silently omit shipped
# work is the same class of defect as one that overclaims, and nothing checked
# it until now.
#
# The store is read directly, and ANTS-4734 re-examined that rather than
# assuming it. `roadmap_query` CAN now return the ids (`shipped_since` on the
# list path, ANTS-4715) — but a shell script reaches a verb only through
# `--remote-json`, which needs a running instance. Migrating would trade
# `sqlite3` — small, present everywhere, and SKIPping loudly when it is not —
# for the GUI app being up, and this gate has to work headless. Read-only, and
# safe while Ants holds its own connection (the store runs in WAL).
#
# ANTS-4759 adds the second question, over the same store and the same file:
# does each CHANGELOG bullet describe what SHIPPED, or does it restate the
# defect? `releases.md` § 2 makes the changelog section the description of what
# shipped. A defect item's roadmap headline states the PROBLEM, so a summary
# copied from it lands under "### Fixed" announcing the bug as though it were
# the release. Byte-identical to the stored headline is the whole test — a
# reworded summary is never flagged, and a copy is legitimate only where the
# headline already reads as a delivered change, which is why this reports
# rather than refuses.
#
# ANTS-4765 adds the third: an item whose body already records a deliberate
# "no CHANGELOG entry" decision is reported as decided rather than uncovered,
# and does not fail the gate. The decision lives on the item because that is
# where it survives the release it was taken for.
#
# Exit 0 = every shipped item in the window is recorded or decided and no
# bullet restates its defect, or the check could not run and said so. Exit 1 =
# undecided uncovered items or copied headlines, listed.
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
            echo "  Also reports [Unreleased] bullets whose bold summary is a"
            echo "  verbatim copy of the item's roadmap headline (ANTS-4759)."
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

# --- Second question (ANTS-4759): does any [Unreleased] bullet's bold summary
#     merely repeat its roadmap headline? Scoped to [Unreleased] because that
#     is the text still open to correction — after `new-rc` rolls it, a wrong
#     summary is in a closed section.
check_copied_headlines() {
    local line id summary headline
    local -a copied=()
    local seen=0 unparsed=0 unrel

    unrel="$(awk '/^## \[Unreleased\]/{f=1;next} /^## \[/{f=0} f' "$CHANGELOG")"
    if [[ -z "${unrel//[[:space:]]/}" ]]; then
        echo "copied-headline:  ⊘ SKIPPED — [Unreleased] is empty, so there is no"
        echo "                  open section to check. Run this before new-rc"
        echo "                  rolls it."
        return 0
    fi

    while IFS= read -r line; do
        [[ "$line" =~ ^[[:space:]]*[-*][[:space:]].*\((ANTS-[0-9]+)[^\)]*\)[[:space:]]*$ ]] || continue
        id="${BASH_REMATCH[1]}"
        seen=$((seen + 1))
        if [[ "$line" =~ ^[[:space:]]*[-*][[:space:]]+\*\*(.+)\*\*[[:space:]]*\(ANTS- ]]; then
            summary="${BASH_REMATCH[1]}"
        else
            unparsed=$((unparsed + 1)); continue
        fi
        headline="$(q "SELECT headline FROM item
                      WHERE project_id=${PROJECT_ID} AND id='${id}' LIMIT 1;")"
        [[ -n "$headline" && "$headline" == "$summary" ]] && copied+=("$id")
    done <<< "$unrel"

    # A count with no flag reads as completeness: say how many id-bearing
    # bullets carried no bold summary and were therefore never compared.
    local note=""
    [[ $unparsed -gt 0 ]] && note=" (skipped ${unparsed} with no bold summary)"

    if [[ ${#copied[@]} -eq 0 ]]; then
        [[ $QUIET -eq 1 ]] || echo "copied-headline:  none of ${seen} cited [Unreleased] bullets repeats its roadmap headline.${note}"
        return 0
    fi

    echo "copied-headline:  ${#copied[@]} of ${seen} cited [Unreleased] bullets have a bold"
    echo "                  summary byte-identical to the roadmap headline${note}:"
    echo
    for id in "${copied[@]}"; do
        printf '  %-12s %s\n' "$id" "$(q "SELECT kind || ' — ' || headline FROM item
                                          WHERE project_id=${PROJECT_ID} AND id='${id}' LIMIT 1;" | cut -c1-88)"
    done
    echo
    echo "A defect item's headline states the PROBLEM, so copying it puts the bug"
    echo "in the release notes where the fix belongs. Reword the summary to say"
    echo "what shipped — the bullet's own body usually already does. A copy is"
    echo "right only where the headline already reads as a delivered change."
    echo
    return 1
}
check_copied_headlines; HEADLINE_RC=$?

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
    exit $HEADLINE_RC
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
    exit $HEADLINE_RC
fi

# --- Which of them a CHANGELOG bullet cites. Position matters: an id in
#     continuation prose is a cross-reference, not a claim that it shipped, so
#     only a line STARTING a bullet counts — the same rule cut-release Phase 0f
#     applies in the other direction.
# --- ANTS-4765: a decision already recorded on the item is not an omission.
#     An item deliberately kept out of the release notes gets that decision
#     written into its own roadmap body, where it travels with the item and
#     outlives any one release report. Without reading it, this gate re-listed
#     every settled decision on every run, and the next session re-triaged the
#     lot to reach answers already on record — which also trains the reader to
#     skim the list the one real entry is hiding in.
declare -A DECIDED_SET=()
while IFS= read -r d; do
    [[ -n "$d" ]] && DECIDED_SET["$d"]=1
done < <(q "SELECT id FROM item
            WHERE project_id=${PROJECT_ID}
              AND body LIKE '%Release note (%no CHANGELOG entry%';")

UNCOVERED=()
DECIDED=()
for ID in "${SHIPPED[@]}"; do
    [[ -z "$ID" ]] && continue
    grep -qE "^[[:space:]]*[-*][[:space:]].*\(${ID}\)" "$CHANGELOG" && continue
    if [[ -n "${DECIDED_SET[$ID]:-}" ]]; then
        DECIDED+=("$ID")
    else
        UNCOVERED+=("$ID")
    fi
done

# Report the decided ones as a count, never silently: a suppression the reader
# cannot see is indistinguishable from a gate that stopped checking.
decided_note() {
    [[ ${#DECIDED[@]} -eq 0 || $QUIET -eq 1 ]] && return 0
    echo "shipped-coverage: ${#DECIDED[@]} more carry a recorded \"no CHANGELOG entry\""
    echo "                  decision in the roadmap and are not counted as uncovered."
}

if [[ ${#UNCOVERED[@]} -eq 0 ]]; then
    [[ $QUIET -eq 1 ]] || echo "shipped-coverage: all ${#SHIPPED[@]} items shipped since ${SINCE} are recorded or decided."
    decided_note
    [[ "${UNDATED:-0}" -gt 0 && $QUIET -eq 0 ]] && \
        echo "                  (${UNDATED} older shipped items carry no date and were not checked.)"
    exit $HEADLINE_RC
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
decided_note
echo
echo "Each is either release-note-worthy — add it with changelog_log — or"
echo "deliberately internal, in which case record that on the item so this gate"
echo "stops asking, by appending a line to its roadmap body of the form:"
echo
echo "  Release note (YYYY-MM-DD): no CHANGELOG entry, deliberately — <reason>"
echo
echo "Write it with roadmap_log op:\"annotate\". Recording it on the item rather"
echo "than in the release report keeps the reasoning with the work."
[[ "${UNDATED:-0}" -gt 0 ]] && \
    echo "(${UNDATED} older shipped items carry no date and were not checked.)"
exit 1
