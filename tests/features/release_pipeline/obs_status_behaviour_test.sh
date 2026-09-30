#!/usr/bin/env bash
#
# Behavioural conformance for INV-18 of tests/features/release_pipeline/spec.md:
# packaging/obs/obs-status.sh judges "the build of revision N is finished" by
# per-repository JOB HISTORY, never by a status word.
#
# Why this exists: two `osc results` status-word watchers misfired on
# 2026-09-30 — one never ended (`succeeded*`), one ended while Fedora was still
# building. The `_result` status says `succeeded` for the PREVIOUS revision
# until the new job starts; only a job-history row with rev >= N proves the
# new build ran.
#
# An `osc` shim on PATH serves canned XML from a per-case directory. It logs
# each call; a file `<name>.<n>` serves the n-th call for that URL, otherwise
# `<name>` serves every call. A missing file makes the shim exit 1.
#
# Exit 0 = every assertion held. Non-zero = a guard regressed.

set -u
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_COMMON_DIR

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
STATUS_SH="${STATUS_SH:-$ROOT/packaging/obs/obs-status.sh}"
[ -f "$STATUS_SH" ] || { echo "obs-status.sh not found at $STATUS_SH" >&2; exit 2; }

PASS=0; FAIL=0
ok()  { echo "  ok   — $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL — $1"; FAIL=$((FAIL + 1)); }
TMPROOT=$(mktemp -d)
trap 'rm -rf "$TMPROOT"' EXIT

PROJ="home:test:proj"; PKG="ants-terminal"; API="https://api.example.invalid"
TW=openSUSE_Tumbleweed; FC=Fedora_42
PASSLINE="100% tests passed, 0 tests failed out of 1234"

mkdir -p "$TMPROOT/bin"
cat > "$TMPROOT/bin/osc" <<'EOF'
#!/usr/bin/env bash
# osc -A <api> api "<url>"
echo "osc $*" >> "$CANNED/osc.log"
url=""; for a in "$@"; do url=$a; done
case "$url" in
  /source/*)          name=source.xml;;
  */_result\?*)       name=result.xml;;
  */_jobhistory\?*)   r=${url#/build/*/}; r=${r%%/_jobhistory*}; name="jobhist_${r//\//_}.xml";;
  */_log\?*)          r=${url#/build/*/}; r=${r%%/${OBS_PACKAGE:-ants-terminal}/_log*}; name="log_${r//\//_}.txt";;
  *) exit 1;;
esac
n=$(cat "$CANNED/.n_$name" 2>/dev/null || echo 0); n=$((n + 1)); echo "$n" > "$CANNED/.n_$name"
if [ -f "$CANNED/$name.$n" ]; then cat "$CANNED/$name.$n"; exit 0; fi
[ -f "$CANNED/$name" ] && { cat "$CANNED/$name"; exit 0; }
echo "404 $name" >&2; exit 1
EOF
chmod +x "$TMPROOT/bin/osc"

# ── canned-data builders ─────────────────────────────────────────────────────
new_case() { CASE=$TMPROOT/$1; rm -rf "$CASE"; mkdir -p "$CASE"; }
source_rev() {                      # $1 rev
    echo "<directory name=\"$PKG\" rev=\"$1\" vrev=\"1\" srcmd5=\"abc\">" > "$CASE/source.xml"
    echo '  <entry name="_service" size="1"/>' >> "$CASE/source.xml"; echo '</directory>' >> "$CASE/source.xml"
}
result() {                          # args: repo:code ...  (arch x86_64)
    {
        echo '<resultlist state="x">'
        for rc in "$@"; do
            echo "  <result project=\"$PROJ\" repository=\"${rc%%:*}\" arch=\"x86_64\" code=\"${rc##*:}\" state=\"${rc##*:}\">"
            echo "    <status package=\"$PKG\" code=\"${rc##*:}\"/>"
            echo '  </result>'
        done
        echo '</resultlist>'
    } > "$CASE/result.xml"
}
jobhist() {                         # $1 repo $2 rev $3 code [$4 seq-suffix]
    local f="$CASE/jobhist_${1}_x86_64.xml${4:+.$4}"
    cat > "$f" <<EOF
<jobhistlist>
  <jobhist package="$PKG" rev="$2" srcmd5="x" versrel="0.7.112-1" bcnt="1" readytime="1" starttime="2" endtime="3" code="$3" uri="u" workerid="w" hostarch="x86_64" reason="source change" verifymd5="y"/>
</jobhistlist>
EOF
}
buildlog() { printf 'building...\n%s\n' "$2" > "$CASE/log_${1}_x86_64.txt"; }
run() {                             # args → sets RC, OUT
    OUT=$(cd "$TMPROOT" && CANNED="$CASE" PATH="$TMPROOT/bin:$PATH" OBS_API="$API" \
            OBS_PROJECT="$PROJ" OBS_PACKAGE="$PKG" POLL_SECS=0 MAX_POLLS="${MAX_POLLS:-3}" \
            timeout 60 bash "$STATUS_SH" "$@" 2>&1); RC=$?
}
check() {                           # label expected-rc [grep-pattern-for-OUT]
    if [ "$RC" -eq "$2" ] && { [ -z "${3-}" ] || grep -q -- "$3" <<<"$OUT"; }; then ok "$1"
    else bad "$1 — expected exit $2${3:+ with /$3/}, got $RC: $OUT"; fi
}

# ── every repository finished the current revision ──────────────────────────
new_case all_done; source_rev 45; result "$TW:unpublished" "$FC:published"
jobhist "$TW" 45 succeeded; jobhist "$FC" 45 succeeded
run; check "INV-18 all repos succeeded at rev 45 → exit 0" 0

# ── the misfire: status says succeeded, job history says an older rev ───────
new_case stale_success; source_rev 45; result "$TW:unpublished" "$FC:published"
jobhist "$TW" 45 succeeded; jobhist "$FC" 44 succeeded
run; check "INV-18 status 'succeeded' but newest job is rev 44: not finished, exit 2" 2

new_case stale_fail; source_rev 45; result "$TW:unpublished" "$FC:building"
jobhist "$TW" 45 succeeded; jobhist "$FC" 44 failed
run; check "INV-18 an older failed row is not a failure, exit 2" 2

# ── eventually finishes: waits rather than judging the first look ───────────
new_case waits; source_rev 45; result "$TW:unpublished" "$FC:building"
jobhist "$TW" 45 succeeded; jobhist "$FC" 44 succeeded 1; jobhist "$FC" 45 succeeded
MAX_POLLS=6 run; check "INV-18 waits: old row first, new row on a later look → exit 0" 0

# ── a failed build of the current revision ──────────────────────────────────
new_case failed; source_rev 45; result "$TW:unpublished" "$FC:failed"
jobhist "$TW" 45 succeeded; jobhist "$FC" 45 failed
run; check "INV-18 rev 45 failed on Fedora → exit 1, names the repository" 1 "$FC"

# ── excluded / disabled repositories are ignored ────────────────────────────
new_case ignored; source_rev 45
result "$TW:unpublished" "Ubuntu_24:excluded" "Debian_12:disabled"
jobhist "$TW" 45 succeeded          # no job history exists for the other two
run; check "INV-18 excluded and disabled repositories ignored → exit 0" 0

# ── --rev N ─────────────────────────────────────────────────────────────────
new_case rev_arg; source_rev 45; result "$TW:unpublished"
jobhist "$TW" 45 succeeded
run --rev 46; check "INV-18 --rev 46 with only rev 45 built → not finished, exit 2" 2
run --rev 45; check "INV-18 --rev 45 with rev 45 built → exit 0" 0
new_case rev_newer; source_rev 45; result "$TW:unpublished"
jobhist "$TW" 50 succeeded
run --rev 45; check "INV-18 a row newer than N counts (rev >= N) → exit 0" 0

# ── --require-tests ─────────────────────────────────────────────────────────
new_case tests_missing; source_rev 45; result "$TW:unpublished"
jobhist "$TW" 45 succeeded; buildlog "$TW" "[  99s] some other output"
run --require-tests; check "INV-18 --require-tests: no ctest summary line → exit 1" 1 "$TW"
run; check "INV-18 without --require-tests the same log is fine → exit 0" 0
new_case tests_present; source_rev 45; result "$TW:unpublished"
jobhist "$TW" 45 succeeded; buildlog "$TW" "$PASSLINE"
run --require-tests; check "INV-18 --require-tests: summary line present → exit 0" 0

# ── results unreadable ──────────────────────────────────────────────────────
new_case unreadable; source_rev 45      # no result.xml: osc exits 1 on every poll
jobhist "$TW" 45 succeeded
run; check "INV-18 _result unreadable through every poll → exit 3" 3

echo
echo "obs_status behavioural: PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
