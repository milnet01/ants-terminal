#!/usr/bin/env bash
#
# Behavioural conformance for INV-22 and INV-23 of
# tests/features/release_pipeline/spec.md: packaging/obs/obs-submit.sh and
# packaging/obs/obs-setup.sh, driven against an `osc` shim (ANTS-6988).
#
# The scripts run with a PATH holding the shims and links to named tools
# only, so the real osc can never run, and git may use the file protocol
# alone, so nothing reaches the network. obs-submit.sh finds its repo from
# its own path, so each case copies it into a throwaway repo whose _service
# points at a local bare "remote".
#
# The osc shim logs each call to $CANNED/osc.log, saves each `meta -F` file
# as $CANNED/meta_<kind>, makes the `.osc` folder on checkout, and records a
# commit's message and file list. OSC_COMMIT_FAILS=n fails the first n
# commits; OSC_META_FAIL=<kind> fails that meta call.
#
# Exit 0 = every assertion held. Non-zero = a guard regressed.

set -u
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_COMMON_DIR

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SUBMIT_SH="${SUBMIT_SH:-$ROOT/packaging/obs/obs-submit.sh}"
SETUP_SH="${SETUP_SH:-$ROOT/packaging/obs/obs-setup.sh}"
SERVICE="$ROOT/packaging/obs/_service"
for f in "$SUBMIT_SH" "$SETUP_SH" "$SERVICE"; do
    [ -f "$f" ] || { echo "not found: $f" >&2; exit 2; }
done

PASS=0; FAIL=0
ok()  { echo "  ok   — $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL — $1"; FAIL=$((FAIL + 1)); }
TMPROOT=$(mktemp -d)
trap 'rm -rf "$TMPROOT"' EXIT

API="https://api.example.invalid"
ENV_BIN=$(type -P env); TIMEOUT_BIN=$(type -P timeout)

# ── the restricted PATH: shims first, then links to these tools alone ───────
TOOLS=$TMPROOT/tools; STUB=$TMPROOT/stub
mkdir -p "$TOOLS" "$STUB" "$TMPROOT/home"
for t in sh bash git sed awk grep mkdir cp rm mktemp dirname basename cat \
         sleep printf ls; do
    p=$(type -P "$t") || { echo "tool not found: $t" >&2; exit 2; }
    ln -s "$p" "$TOOLS/$t"
done

cat > "$STUB/osc" <<'EOF'
#!/usr/bin/env bash
# osc -A <api> <subcommand> ...
echo "osc $*" >> "$CANNED/osc.log"
[ "${1:-}" = "-A" ] || { echo "osc shim: no -A" >&2; exit 9; }
shift 2
case "$1" in
  meta)       # meta <kind> <project> [<package>] -F <file>
      kind=$2; f=""; prev=""
      for a in "$@"; do [ "$prev" = "-F" ] && f=$a; prev=$a; done
      [ "${OSC_META_FAIL:-}" = "$kind" ] && { echo "osc shim: meta $kind refused" >&2; exit 1; }
      cp "$f" "$CANNED/meta_$kind";;
  checkout)   mkdir -p "$2/$3/.osc";;
  update|add|addremove) ;;
  commit)     # commit -m <message>
      n=$(( $(cat "$CANNED/.commits" 2>/dev/null || echo 0) + 1 ))
      echo "$n" > "$CANNED/.commits"
      [ "$n" -le "${OSC_COMMIT_FAILS:-0}" ] && { echo "osc shim: out of date" >&2; exit 1; }
      echo "$3" > "$CANNED/commit_msg"
      ls -A > "$CANNED/committed_files";;
  *) echo "osc shim: unexpected call: $*" >&2; exit 9;;
esac
EOF
# rpmspec -P <spec>: the fixture spec has no macros to expand.
printf '#!/usr/bin/env bash\ncat "$2"\n' > "$STUB/rpmspec"
chmod +x "$STUB/osc" "$STUB/rpmspec"

g() {
    HOME="$TMPROOT/home" GIT_CONFIG_NOSYSTEM=1 git -c user.name=t \
        -c user.email=t@example.invalid -c init.defaultBranch=main "$@"
}

# A throwaway repo whose tag v1.2.3 is on a local bare remote, with
# origin/main at HEAD. Sets CASE, REPO, REMOTE.
new_case() {
    CASE=$TMPROOT/$1; REPO=$CASE/repo; REMOTE=$CASE/remote.git
    mkdir -p "$REPO/packaging/obs" "$REPO/packaging/opensuse" "$CASE/tmp"
    cp "$SUBMIT_SH" "$REPO/packaging/obs/"
    sed -e "s|<param name=\"url\">[^<]*</param>|<param name=\"url\">$REMOTE</param>|" \
        -e "s|<param name=\"revision\">[^<]*</param>|<param name=\"revision\">v1.2.3</param>|" \
        "$SERVICE" > "$REPO/packaging/obs/_service"
    printf 'Name: ants-terminal\nVersion:        0.0.1\n%%build\necho build\n%%check\necho check\n' \
        > "$REPO/packaging/opensuse/ants-terminal.spec"
    echo "# rpmlintrc" > "$REPO/packaging/opensuse/ants-terminal-rpmlintrc"
    echo "--- a/x" > "$REPO/packaging/opensuse/keep.patch"
    g -C "$REPO" init -q && g -C "$REPO" add -A && g -C "$REPO" commit -qm init \
        && g -C "$REPO" tag v1.2.3 && g clone -q --bare "$REPO" "$REMOTE" \
        && g -C "$REPO" update-ref refs/remotes/origin/main HEAD \
        || { echo "fixture repo failed" >&2; exit 2; }
}
co() { echo "$REPO/build-obs/$1/ants-terminal"; }

# args → sets RC, OUT. $1 is the script; PATH_OVERRIDE replaces the PATH.
run_script() {
    local script=$1; shift
    OUT=$(cd "$CASE" && "$ENV_BIN" -i HOME="$CASE" LANG=C TMPDIR="$CASE/tmp" \
            PATH="${PATH_OVERRIDE:-$STUB:$TOOLS}" CANNED="$CASE" \
            OSC_COMMIT_FAILS="${OSC_COMMIT_FAILS:-0}" OSC_META_FAIL="${OSC_META_FAIL:-}" \
            GIT_CONFIG_NOSYSTEM=1 GIT_ALLOW_PROTOCOL=file GIT_TERMINAL_PROMPT=0 \
            OBS_API="$API" OBS_RETRY_SECS=0 ${EXTRA_ENV:-} \
            "$TIMEOUT_BIN" 60 "$TOOLS/sh" "$script" "$@" 2>&1); RC=$?
}
submit() { run_script "$REPO/packaging/obs/obs-submit.sh" "$@"; }
setup()  { run_script "$SETUP_SH"; }
check() {                           # label expected-rc [grep-pattern-for-OUT]
    if [ "$RC" -eq "$2" ] && { [ -z "${3-}" ] || grep -q -- "$3" <<<"$OUT"; }; then ok "$1"
    else bad "$1 — expected exit $2${3:+ with /$3/}, got $RC: $OUT"; fi
}
expect() {                          # label command...
    if "${@:2}"; then ok "$1"; else bad "$1"; fi
}
nogrep() { ! grep -q -- "$1" "${@:2}"; }   # pattern files...: no line matches
commits() { cat "$CASE/.commits" 2>/dev/null || echo 0; }   # the shim's count

# ── INV-22: obs-submit.sh ───────────────────────────────────────────────────
new_case release; submit
check "INV-22 release submit → exit 0" 0
C=$(co home:milnet:ants-terminal)
expect "INV-22 a first run checks the package out" grep -q " checkout home:milnet:ants-terminal ants-terminal" "$CASE/osc.log"
expect "INV-22 Version: is stamped from the tag" grep -qx "Version:        1.2.3" "$C/ants-terminal.spec"
expect "INV-22 _service is committed as written" cmp -s "$REPO/packaging/obs/_service" "$C/_service"
expect "INV-22 the rpmlintrc and the repo's patch are carried" \
    test -f "$C/ants-terminal-rpmlintrc" -a -f "$C/keep.patch"
expect "INV-22 the commit names the tag" grep -qx "ants-terminal v1.2.3" "$CASE/commit_msg"

new_case update; C=$(co home:milnet:ants-terminal); mkdir -p "$C/.osc"
echo "--- old" > "$C/old.patch"
submit; check "INV-22 an existing checkout is updated → exit 0" 0
expect "INV-22 an existing checkout is updated, not checked out again" \
    nogrep ' checkout ' "$CASE/osc.log"
expect "INV-22 ... through osc update" grep -q ' update$' "$CASE/osc.log"
expect "INV-22 a patch deleted from the repo leaves the package" test ! -e "$C/old.patch"

new_case staging; SHA=$(g -C "$REPO" rev-parse HEAD)
submit --staging "$SHA" 9.9.9; check "INV-22 staging submit → exit 0" 0
C=$(co home:milnet:ants-terminal-staging)
expect "INV-22 staging aims _service at the commit" \
    grep -q "<param name=\"revision\">$SHA</param>" "$C/_service"
expect "INV-22 staging gives the version literally" \
    grep -q '<param name="versionformat">9.9.9</param>' "$C/_service"
expect "INV-22 staging drops match-tag and versionrewrite" \
    nogrep 'match-tag\|versionrewrite-' "$C/_service"
expect "INV-22 staging stamps its version" grep -qx "Version:        9.9.9" "$C/ants-terminal.spec"

new_case unpushed_commit
echo x > "$REPO/x"; g -C "$REPO" add x; g -C "$REPO" commit -qm later
submit --staging "$(g -C "$REPO" rev-parse HEAD)" 9.9.9
check "INV-22 a staging commit not on origin/main → exit 1" 1 "not on origin/main"
expect "INV-22 ... and osc is never called" test ! -e "$CASE/osc.log"

new_case no_local_tag; g -C "$REPO" tag -d v1.2.3 >/dev/null; submit
check "INV-22 the pinned tag missing locally → exit 1" 1 "does not exist locally"
expect "INV-22 ... and osc is never called" test ! -e "$CASE/osc.log"

new_case no_remote_tag; g -C "$REMOTE" tag -d v1.2.3 >/dev/null; submit
check "INV-22 the pinned tag missing on the remote → exit 1" 1 "is not on"
expect "INV-22 ... and osc is never called" test ! -e "$CASE/osc.log"

new_case usage
submit --staging onlysha; check "INV-22 --staging without a version → exit 2" 2 "usage"
submit extra;             check "INV-22 an unknown argument → exit 2" 2 "usage"

new_case dash_version; submit --staging "$(g -C "$REPO" rev-parse HEAD)" 1.2.3-rc1
check "INV-22 a version holding '-' → exit 1" 1 "not a legal rpm version"
expect "INV-22 ... and nothing is committed" test "$(commits)" -eq 0

new_case retry; OSC_COMMIT_FAILS=2 submit
check "INV-22 a commit refused twice is retried → exit 0" 0
expect "INV-22 ... three commit attempts" test "$(commits)" -eq 3

new_case give_up; OSC_COMMIT_FAILS=99 submit
check "INV-22 a commit refused every time → exit 1" 1 "giving up"
expect "INV-22 ... after five attempts" test "$(commits)" -eq 5

new_case no_osc; PATH_OVERRIDE="$TOOLS" submit
check "INV-22 osc not installed → exit 1" 1 "osc not installed"

# ── INV-23: obs-setup.sh ────────────────────────────────────────────────────
new_case setup
EXTRA_ENV="OBS_PROJECT=home:test:proj OBS_PACKAGE=pkg OBS_USER=tester" setup
check "INV-23 setup → exit 0" 0
expect "INV-23 the metas go project, prjconf, package, in that order" \
    test "$(sed 's/^osc -A [^ ]* meta \([a-z]*\) .*/\1/' "$CASE/osc.log" | tr '\n' ' ')" = "prj prjconf pkg "
expect "INV-23 the project meta names the project" grep -q '<project name="home:test:proj">' "$CASE/meta_prj"
expect "INV-23 the project meta names the maintainer" grep -q 'userid="tester"' "$CASE/meta_prj"
expect "INV-23 the package meta names package and project" \
    grep -q '<package name="pkg" project="home:test:proj">' "$CASE/meta_pkg"
FEDORA_ARM=$(grep -A1 '^%if "%_repository" == "Fedora_44"$' "$CASE/meta_prjconf")
expect "INV-23 the wget Prefer is scoped to Fedora" \
    test "$(sed -n 2p <<<"$FEDORA_ARM")" = "Prefer: wget2-wget"
expect "INV-23 the metas hold no double hyphen" nogrep '--' "$CASE/meta_prj" "$CASE/meta_pkg"
if XMLLINT=$(type -P xmllint); then
    expect "INV-23 both metas are well-formed XML" "$XMLLINT" --noout --nonet "$CASE/meta_prj" "$CASE/meta_pkg"
fi

new_case setup_fails; OSC_META_FAIL=prj setup
expect "INV-23 a refused project meta stops the script" test "$RC" -ne 0 -a ! -e "$CASE/meta_pkg"

new_case setup_no_osc; PATH_OVERRIDE="$TOOLS" setup
check "INV-23 osc not installed → exit 1" 1 "osc not installed"

echo
echo "obs_submit behavioural: PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
