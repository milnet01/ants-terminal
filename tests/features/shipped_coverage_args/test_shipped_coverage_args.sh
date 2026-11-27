#!/bin/bash
# ANTS-6631 — tools/check-shipped-coverage.sh refuses a bad --since and finds
# a project whose checkout path holds a '. See spec.md.
#
# Runs the real script against a throwaway repo and a throwaway store
# (XDG_DATA_HOME), never the real roadmap store.

set -uo pipefail
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_COMMON_DIR

SCRIPT="$(cd "$(dirname "$0")/../../.." && pwd)/tools/check-shipped-coverage.sh"
command -v git >/dev/null 2>&1 || { echo "SKIP: git not available"; exit 77; }
command -v sqlite3 >/dev/null 2>&1 || { echo "SKIP: sqlite3 not available"; exit 77; }

failures=0
check() {  # check <description> <condition-result>
    if [[ "$2" == "0" ]]; then
        echo "  ok   — $1"
    else
        echo "  FAIL — $1"
        failures=$((failures + 1))
    fi
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# A repo whose path holds a ', registered in a scratch store with one item
# shipped in the window.
repo="$tmp/it's repo"
mkdir -p "$repo" "$tmp/data/ants-terminal"
git init -q "$repo"
cat > "$repo/CHANGELOG.md" <<'EOF'
# Changelog

## [Unreleased]

### Fixed

- **Something else shipped.** (ANTS-2)
EOF
db="$tmp/data/ants-terminal/roadmap.sqlite"
root_sql=${repo//"'"/"''"}
sqlite3 "$db" "
    CREATE TABLE project (project_id INTEGER PRIMARY KEY, root TEXT);
    CREATE TABLE item (project_id INTEGER, id TEXT, kind TEXT, headline TEXT,
                       status TEXT, shipped TEXT, body TEXT);
    INSERT INTO project VALUES (1, '$root_sql');
    INSERT INTO item VALUES (1, 'ANTS-1', 'fix', 'A thing shipped.',
                             'shipped', '2026-02-01', '');"

run() {  # run <args...>: sets $out and $rc; a hang ends at 10 s as rc 124
    out=$(cd "$repo" && XDG_DATA_HOME="$tmp/data" timeout 10 bash "$SCRIPT" "$@" 2>&1)
    rc=$?
}

echo "--since with no value is refused, not looped on"
run --since
check "exit 2 (got $rc)" "$([[ $rc -eq 2 ]] && echo 0 || echo 1)"

echo "--since that is not a date is refused before it reaches SQL"
run --since "2026-01-01' OR '1'='1"
check "exit 2 (got $rc)" "$([[ $rc -eq 2 ]] && echo 0 || echo 1)"

echo "a checkout path holding a ' still finds its project"
run --since 2026-01-01
check "exit 1: the uncited item is reported (got $rc)" \
      "$([[ $rc -eq 1 ]] && echo 0 || echo 1)"
check "the report names ANTS-1" "$(grep -q 'ANTS-1' <<<"$out" && echo 0 || echo 1)"
check "the gate did not skip" "$(grep -q 'SKIPPED' <<<"$out" && echo 1 || echo 0)"

echo "control: once the CHANGELOG cites it, the gate passes"
sed -i 's/(ANTS-2)/(ANTS-1, ANTS-2)/' "$repo/CHANGELOG.md"
run --since 2026-01-01
check "exit 0 (got $rc)" "$([[ $rc -eq 0 ]] && echo 0 || echo 1)"

if [[ $failures -gt 0 ]]; then
    echo "FAILED: $failures assertion(s)"
    exit 1
fi
echo "PASS"
exit 0
