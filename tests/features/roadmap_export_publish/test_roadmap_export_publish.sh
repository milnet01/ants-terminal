#!/usr/bin/env bash
# ANTS-3794 INV-7, INV-8, INV-9, INV-10, INV-12, INV-13, INV-14, INV-15 — tools/roadmap-export-publish.sh
# and the snapshot record. Contract: tests/features/roadmap_export_publish/spec.md
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
PUBLISH="$ROOT/tools/roadmap-export-publish.sh"
SNAPSHOT="$ROOT/tools/roadmap-store-backup.sh"

for tool in git flock sqlite3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "SKIP: $tool is not installed"
        exit 77
    fi
done

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
check() {  # check <description> <command...>
    local what=$1; shift
    if "$@"; then echo "ok   - $what"; else echo "FAIL - $what"; fails=$((fails + 1)); fi
}

# Isolation: no global or system git config (so no machine-wide hooks), a
# private state dir, and a notify-send that records instead of notifying.
printf '[user]\n\tname = t\n\temail = t@t\n[init]\n\tdefaultBranch = main\n' > "$T/gitconfig"
export GIT_CONFIG_GLOBAL="$T/gitconfig" GIT_CONFIG_NOSYSTEM=1
export XDG_STATE_HOME="$T/state"
mkdir -p "$T/bin"
printf '#!/bin/sh\necho "$*" >> "%s/notified"\n' "$T" > "$T/bin/notify-send"

# The stub binary: writes one export and its lock file, notes that it ran,
# and exits with $STUB_EXIT.
cat > "$T/bin/ants-stub" <<EOF
#!/bin/sh
[ "\$1" = --export-roadmaps ] || exit 9
mkdir -p "\$2"
echo "{\"n\":\"\$(date +%s%N)\"}" > "\$2/alpha.jsonl"
: > "\$2/alpha.jsonl.lock"
: > "$T/stub-ran"
exit "\${STUB_EXIT:-0}"
EOF
chmod +x "$T/bin/"*
export PATH="$T/bin:$PATH" ANTS_ROADMAP_EXPORT_BIN="$T/bin/ants-stub"

REC="$T/state/ants-terminal/roadmap-backup-export.state"
recval() { sed -n "s/^$1=//p" "$2"; }

git init -q --bare "$T/remote.git"
git clone -q "$T/remote.git" "$T/a" 2>/dev/null
echo base > "$T/a/other.txt"
git -C "$T/a" add other.txt && git -C "$T/a" commit -qm base && git -C "$T/a" push -q origin main

# --- INV-7 and INV-9 (success) ---
echo staged > "$T/a/other.txt" && git -C "$T/a" add other.txt
echo unstaged > "$T/a/loose.txt"
"$PUBLISH" "$T/a" >/dev/null 2>&1
rc=$?
check "INV-7 publish succeeds" test "$rc" -eq 0
check "INV-7 commit holds only the export" \
    test "$(git -C "$T/a" show --name-only --format= HEAD)" = "roadmap-export/alpha.jsonl"
check "INV-7 staged change still staged" \
    test "$(git -C "$T/a" status --porcelain -- other.txt)" = "M  other.txt"
check "INV-7 unstaged file untouched" \
    test "$(git -C "$T/a" status --porcelain -- loose.txt)" = "?? loose.txt"
check "INV-7 lock file not tracked" \
    test -z "$(git -C "$T/a" ls-files -- roadmap-export/alpha.jsonl.lock)"
check "INV-7 pushed" \
    test "$(git -C "$T/a" rev-parse HEAD)" = "$(git -C "$T/remote.git" rev-parse main)"
check "INV-9 success empties error" test -z "$(recval error "$REC")"
check "INV-9 success sets success" test -n "$(recval success "$REC")"

# --- INV-9 (nothing to commit still advances success) ---
sed -i 's/^success=.*/success=2000-01-01T00:00:00Z/' "$REC"
git -C "$T/a" update-index --assume-unchanged roadmap-export/alpha.jsonl
"$PUBLISH" "$T/a" >/dev/null 2>&1
check "INV-9 nothing-to-commit exits 0" test $? -eq 0
check "INV-9 nothing-to-commit advances success" \
    test "$(recval success "$REC")" != "2000-01-01T00:00:00Z"
git -C "$T/a" update-index --no-assume-unchanged roadmap-export/alpha.jsonl

# --- INV-9 (failure) ---
before=$(recval success "$REC")
head=$(git -C "$T/a" rev-parse HEAD)
STUB_EXIT=1 "$PUBLISH" "$T/a" >/dev/null 2>&1
check "INV-9 failed export exits non-zero" test $? -ne 0
check "INV-9 failure sets error" test -n "$(recval error "$REC")"
check "INV-9 failure keeps success" test "$(recval success "$REC")" = "$before"
check "INV-9 failure notifies" test -s "$T/notified"
check "INV-9 failure makes no commit" test "$(git -C "$T/a" rev-parse HEAD)" = "$head"
git -C "$T/a" checkout -q -- roadmap-export/alpha.jsonl

# --- INV-8 (diverged upstream) ---
git clone -q "$T/remote.git" "$T/b" 2>/dev/null
echo other > "$T/b/elsewhere.txt"
git -C "$T/b" add elsewhere.txt && git -C "$T/b" commit -qm elsewhere && git -C "$T/b" push -q origin main
rm -f "$T/stub-ran"
head=$(git -C "$T/a" rev-parse HEAD)
remote=$(git -C "$T/remote.git" rev-parse main)
"$PUBLISH" "$T/a" >"$T/inv8.log" 2>&1
check "INV-8 diverged upstream fails" test $? -ne 0
check "INV-8 export never ran" test ! -e "$T/stub-ran"
check "INV-8 no commit" test "$(git -C "$T/a" rev-parse HEAD)" = "$head"
check "INV-8 no push" test "$(git -C "$T/remote.git" rev-parse main)" = "$remote"
check "INV-8 error names the divergence" grep -q "Not merging" "$REC"

# --- INV-10 (lock held) ---
cp "$REC" "$T/rec.before"
head=$(git -C "$T/a" rev-parse HEAD)
exec 7>"$T/state/ants-terminal/roadmap-backup-export.lock"
flock -n 7
"$PUBLISH" "$T/a" >/dev/null 2>&1
check "INV-10 held lock exits 3" test $? -eq 3
exec 7>&-
check "INV-10 record unchanged" cmp -s "$REC" "$T/rec.before"
check "INV-10 repository unchanged" test "$(git -C "$T/a" rev-parse HEAD)" = "$head"

# --- INV-12 (snapshot record) ---
SREC="$T/state/ants-terminal/roadmap-backup-snapshot.state"
mkdir -p "$T/data/ants-terminal"
sqlite3 "$T/data/ants-terminal/roadmap.sqlite" "create table t(a); insert into t values (1);"
XDG_DATA_HOME="$T/data" "$SNAPSHOT" "$T/snaps" >/dev/null 2>&1
check "INV-12 snapshot succeeds" test $? -eq 0
check "INV-12 success sets success" test -n "$(recval success "$SREC")"
check "INV-12 success empties error" test -z "$(recval error "$SREC")"
check "INV-12 success sets dest to the canonical folder" test "$(recval dest "$SREC")" = "$(cd "$T/snaps" && pwd -P)"
before=$(recval success "$SREC")
rm "$T/data/ants-terminal/roadmap.sqlite"
XDG_DATA_HOME="$T/data" "$SNAPSHOT" "$T/snaps" >/dev/null 2>&1
check "INV-12 missing store fails" test $? -ne 0
check "INV-12 failure sets error" test -n "$(recval error "$SREC")"
check "INV-12 failure keeps success" test "$(recval success "$SREC")" = "$before"
check "INV-12 failure keeps dest" test "$(recval dest "$SREC")" = "$(cd "$T/snaps" && pwd -P)"
check "INV-12 record always carries dest=" grep -q "^dest=" "$SREC"

# --- INV-13 (ANTS-5621: the store the snapshot copied) ---
sqlite3 "$T/data/ants-terminal/roadmap.sqlite" "create table t(a); insert into t values (1);"
XDG_DATA_HOME="$T/data" "$SNAPSHOT" "$T/snaps" >/dev/null 2>&1
want="$(cd "$T/data/ants-terminal" && pwd -P)/roadmap.sqlite"
check "INV-13 success sets store to the copied store" test "$(recval store "$SREC")" = "$want"
mkdir -p "$T/other"
XDG_DATA_HOME="$T/other" "$SNAPSHOT" "$T/snaps" >/dev/null 2>&1
check "INV-13 a run with no store fails" test $? -ne 0
check "INV-13 failure under another store keeps store" test "$(recval store "$SREC")" = "$want"

# --- INV-14 (ANTS-6311: the know-how store) ---
KDATA="$T/kdata/ants-terminal"
KDEST="$T/ksnaps"
KSTORE="$KDATA/knowhow.sqlite"
snapshot_k() { XDG_DATA_HOME="$T/kdata" "$SNAPSHOT" "$KDEST" "$@" >/dev/null 2>&1; }
mkdir -p "$KDATA"
sqlite3 "$KDATA/roadmap.sqlite" "create table t(a); insert into t values (1);"
snapshot_k
check "INV-14 no know-how store still succeeds" test $? -eq 0
check "INV-14 no know-how store, no copy" test -z "$(find "$KDEST" -name 'knowhow-*')"

sqlite3 "$KSTORE" "create table k(a); insert into k values ('kept');"
snapshot_k
check "INV-14 a store is copied, exit 0" test $? -eq 0
kcopy=$(find "$KDEST" -name 'knowhow-*.sqlite' | sort | tail -n 1)
check "INV-14 the copy holds the rows" test "$(sqlite3 "${kcopy:-/nonexistent}" 'select a from k;' 2>/dev/null)" = kept
check "INV-14 the copy is mode 600" test "$(stat -c %a "${kcopy:-/nonexistent}" 2>/dev/null)" = 600

: > "$KDEST/knowhow-20000101-000000.sqlite"
: > "$KDEST/roadmap-20000101-000000.sqlite"
snapshot_k 1
check "INV-14 KEEP prunes old know-how copies" test ! -e "$KDEST/knowhow-20000101-000000.sqlite"
check "INV-14 KEEP leaves the newest know-how copy" test "$(find "$KDEST" -name 'knowhow-*.sqlite' | wc -l)" -eq 1
check "INV-14 KEEP leaves the newest roadmap copy" test "$(find "$KDEST" -name 'roadmap-*.sqlite' | wc -l)" -eq 1

rm -f "$T/notified"
sed -i 's/^success=.*/success=2000-01-01T00:00:00Z/' "$SREC"
printf 'not a database' > "$KSTORE"
sleep 1  # snapshot names resolve to the second
snapshot_k 8
check "INV-14 a failed copy exits non-zero" test $? -ne 0
check "INV-14 a failed copy sets error" test -n "$(recval error "$SREC")"
check "INV-14 a failed copy notifies" test -s "$T/notified"
check "INV-14 the roadmap copy still counts as a success" test "$(recval success "$SREC")" != "2000-01-01T00:00:00Z"
check "INV-14 the roadmap copy is kept" test "$(find "$KDEST" -name 'roadmap-*.sqlite' | wc -l)" -eq 2
check "INV-14 a failed copy leaves no partial" test -z "$(find "$KDEST" -name '*.partial')"

# --- INV-15 (ANTS-5426: a shrunken copy holds the full snapshots) ---
SDATA="$T/sdata/ants-terminal"
SDEST="$T/ssnaps"
SSTORE="$SDATA/roadmap.sqlite"
mkdir -p "$SDATA" "$SDEST"
mkstore() {  # mkstore <file> <table> <rows>
    rm -f "$1"
    sqlite3 "$1" "create table $2(a); with recursive n(i) as (select 1 union all select i+1 from n where i < $3) insert into $2 select i from n where $3 > 0;"
}
snapshot_s() { XDG_DATA_HOME="$T/sdata" "$SNAPSHOT" "$SDEST" "$@" >/dev/null 2>&1; }
nsnaps() { find "$SDEST" -name "$1-*.sqlite" | wc -l; }

# The boundary: exactly half is not a shrink, so KEEP prunes as before.
mkstore "$SDEST/roadmap-20000101-000000.sqlite" item 10
mkstore "$SSTORE" item 5
snapshot_s 1
check "INV-15 exactly half exits 0" test $? -eq 0
check "INV-15 exactly half prunes as before" test "$(nsnaps roadmap)" -eq 1

# Under half: the full snapshots stay, the small ones still rotate.
rm -f "$SDEST"/roadmap-*.sqlite "$T/notified"
mkstore "$SDEST/roadmap-20000101-000000.sqlite" item 10
mkstore "$SDEST/roadmap-20000102-000000.sqlite" item 3
mkstore "$SDEST/roadmap-20000103-000000.sqlite" item 10
mkstore "$SSTORE" item 2
sed -i 's/^success=.*/success=2000-01-01T00:00:00Z/' "$SREC"
sleep 1
snapshot_s 1
check "INV-15 a shrunken copy exits non-zero" test $? -ne 0
check "INV-15 the error names the shrink" grep -q "^error=.*roadmap.*2 item.*10" "$SREC"
check "INV-15 a shrunken copy notifies" test -s "$T/notified"
check "INV-15 a shrunken copy does not advance success" test "$(recval success "$SREC")" = "2000-01-01T00:00:00Z"
check "INV-15 the oldest full snapshot is held" test -e "$SDEST/roadmap-20000101-000000.sqlite"
check "INV-15 the newer full snapshot is held" test -e "$SDEST/roadmap-20000103-000000.sqlite"
check "INV-15 a small old snapshot still rotates" test ! -e "$SDEST/roadmap-20000102-000000.sqlite"
check "INV-15 the shrunken copy is kept too" test "$(nsnaps roadmap)" -eq 3

# A second plain run changes nothing: the hold lasts until the switch.
sleep 1
snapshot_s 1
check "INV-15 the hold lasts across runs" test $? -ne 0
check "INV-15 full snapshots still held" test -e "$SDEST/roadmap-20000101-000000.sqlite" -a -e "$SDEST/roadmap-20000103-000000.sqlite"

# The switch accepts the shrink once; later plain runs then pass.
sleep 1
ROADMAP_BACKUP_ACCEPT_SHRINK=1 snapshot_s 4
check "INV-15 the switch exits 0" test $? -eq 0
check "INV-15 the switch advances success" test "$(recval success "$SREC")" != "2000-01-01T00:00:00Z"
check "INV-15 the switch prunes to KEEP" test "$(nsnaps roadmap)" -eq 4
check "INV-15 a full snapshot inside KEEP survives the switch" test -e "$SDEST/roadmap-20000103-000000.sqlite"
sleep 1
snapshot_s 4
check "INV-15 an accepted shrink stays accepted" test $? -eq 0

# A further halving is a new shrink, and the old acceptance does not cover it.
mkstore "$SSTORE" item 0
sleep 1
snapshot_s 4
check "INV-15 a second shrink fails again" test $? -ne 0

# The know-how store follows the same rule on its own table, and a roadmap
# success still counts.
rm -f "$SDEST"/roadmap-*.sqlite "$SDEST"/.*-shrink-accepted
mkstore "$SSTORE" item 4
mkstore "$SDEST/knowhow-20000101-000000.sqlite" know_how 10
mkstore "$SDATA/knowhow.sqlite" know_how 1
sed -i 's/^success=.*/success=2000-01-01T00:00:00Z/' "$SREC"
sleep 1
snapshot_s 1
check "INV-15 a shrunken know-how copy exits non-zero" test $? -ne 0
check "INV-15 the full know-how snapshot is held" test -e "$SDEST/knowhow-20000101-000000.sqlite"
check "INV-15 the roadmap copy still counts as a success" test "$(recval success "$SREC")" != "2000-01-01T00:00:00Z"

echo "$fails failure(s)"
[ "$fails" -eq 0 ]
