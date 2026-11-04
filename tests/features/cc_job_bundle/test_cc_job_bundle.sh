#!/usr/bin/env bash
# ANTS-5629 — install the cc-job component into a scratch prefix and run the
# installed copy. No systemd --user is needed: a job starts through a stand-in.
# Args: <build dir>.
set -euo pipefail

build="$1"

prefix="$(mktemp -d "${build}/cc-job-bundle.XXXXXX")"
trap 'rm -rf "$prefix"' EXIT

cmake --install "$build" --component cc-job --prefix "$prefix" >/dev/null

# INV-1 — an executable bin/cc-job under the prefix.
bin="$prefix/bin/cc-job"
[ -x "$bin" ] || { echo "FAIL INV-1: no executable bin/cc-job under $prefix"; exit 1; }

# INV-2 — --help names what it needs.
help="$("$bin" --help)"
for need in "systemd --user" "Konsole"; do
    case "$help" in
        *"$need"*) ;;
        *) echo "FAIL INV-2: --help does not name '$need'"; exit 1 ;;
    esac
done

# INV-3 — with CC_JOB_DIR unset, the log dir is $XDG_STATE_HOME/cc-job.
state="$prefix/state"
out="$(env -u CC_JOB_DIR XDG_STATE_HOME="$state" "$bin" status)"
[ "$out" = "no jobs" ] || { echo "FAIL INV-3: status printed '$out'"; exit 1; }
[ -d "$state/cc-job" ] || { echo "FAIL INV-3: $state/cc-job was not created"; exit 1; }

# INV-4 — watch without Konsole exits 2 with a message naming it. PATH holds
# only mkdir, the one external command the script runs before watch's check.
: >"$state/cc-job/probe.log"
tools="$prefix/tools"
mkdir "$tools"
ln -s "$(command -v mkdir)" "$tools/mkdir"
bash_bin="$(command -v bash)"
set +e
msg="$(env -u CC_JOB_DIR XDG_STATE_HOME="$state" PATH="$tools" \
           "$bash_bin" "$bin" watch probe 2>&1)"
rc=$?
set -e
[ "$rc" -eq 2 ] || { echo "FAIL INV-4: watch without Konsole exited $rc, want 2"; exit 1; }
case "$msg" in
    *Konsole*) ;;
    *) echo "FAIL INV-4: watch without Konsole printed '$msg'"; exit 1 ;;
esac

# INV-5..7 — a job started through a stand-in systemd-run, which gives the job
# an empty environment apart from PATH, as a real unit's is apart from the user
# manager's. The stand-in records its arguments and the variables file's mode.
fake="$prefix/fake"
jobs="$prefix/jobs"
mkdir "$fake" "$jobs"
cat >"$fake/systemctl" <<'EOF'
#!/bin/bash
exit 1
EOF
cat >"$fake/systemd-run" <<EOF
#!/bin/bash
printf '%s\n' "\$*" >"$prefix/argv"
stat -c %a "$jobs/probe.env" >"$prefix/mode"
while [[ \$1 == --* ]]; do shift; done
exec env -i PATH="\$PATH" "\$@"
EOF
chmod +x "$fake/systemctl" "$fake/systemd-run"

value=$'two words "quoted" $notexpanded\nsecond line'
env CC_JOB_DIR="$jobs" PATH="$fake:$PATH" PROBE_VALUE="$value" \
    CLAUDE_CODE_SESSION_ID=session-leak ANTS_MCP_SOCKET=session-leak \
    "$bin" start probe -- bash -c \
    'printf "%s" "$PROBE_VALUE" >"$0"; printf "%s" "${CLAUDE_CODE_SESSION_ID:-}${ANTS_MCP_SOCKET:-}" >"$0.session"' \
    "$prefix/seen" >/dev/null

[ "$(cat "$prefix/seen")" = "$value" ] ||
    { echo "FAIL INV-5: the job saw '$(cat "$prefix/seen")', want the exported value"; exit 1; }
[ ! -s "$prefix/seen.session" ] ||
    { echo "FAIL INV-6: a session variable reached the job"; exit 1; }
[ "$(cat "$prefix/mode")" = 600 ] ||
    { echo "FAIL INV-7: the variables file had mode $(cat "$prefix/mode"), want 600"; exit 1; }
[ ! -e "$jobs/probe.env" ] ||
    { echo "FAIL INV-7: the variables file outlived the job's start"; exit 1; }
case "$(cat "$prefix/argv")" in
    *notexpanded*) echo "FAIL INV-7: an exported value is on systemd-run's command line"; exit 1 ;;
esac
case "$help" in
    *"xported"*) ;;
    *) echo "FAIL INV-5: --help does not say which variables reach a job"; exit 1 ;;
esac

# INV-8..10 — plain status over more jobs than it shows. A stand-in systemctl
# counts is-active calls: 26 jobs must cost none of them.
many="$prefix/many"
mkdir "$many"
for i in $(seq -w 1 25); do
    : >"$many/job$i.log"
    echo 0 >"$many/job$i.exit"
    echo "started=2026-01-$i" >"$many/job$i.info"
    touch -d "2026-01-$i 12:00:00" "$many/job$i.log"
done
: >"$many/oldrun.log"
touch -d "2020-01-01 12:00:00" "$many/oldrun.log"
calls="$prefix/is-active-calls"
: >"$calls"
listing="$prefix/listing"
cat >"$fake/systemctl" <<EOF
#!/bin/bash
for a in "\$@"; do
    case "\$a" in
        list-units) [ -f "$listing" ] || exit 1
                    echo "cc-job-oldrun.service loaded active running stand-in"; exit 0 ;;
        is-active)  echo x >>"$calls"
                    case "\$*" in *cc-job-oldrun.service*) exit 0 ;; esac; exit 3 ;;
    esac
done
exit 1
EOF
: >"$listing"
out="$(env CC_JOB_DIR="$many" PATH="$fake:$PATH" "$bin" status)"
[ ! -s "$calls" ] || { echo "FAIL INV-8: plain status ran is-active $(wc -l <"$calls") times"; exit 1; }
[ "$(sed -n 1p <<<"$out" | cut -d: -f1)" = oldrun ] ||
    { echo "FAIL INV-9: a running job did not come first: $out"; exit 1; }
sed -n 1p <<<"$out" | grep -q 'oldrun: running' ||
    { echo "FAIL INV-9: the running job read as not running: $out"; exit 1; }
[ "$(sed -n 2p <<<"$out" | cut -d: -f1)" = job25 ] ||
    { echo "FAIL INV-9: the newest finished job did not follow it: $out"; exit 1; }
[ "$(sed -n 21p <<<"$out" | cut -d: -f1)" = job06 ] ||
    { echo "FAIL INV-9: finished jobs are not the newest 20: $out"; exit 1; }
sed -n 22p <<<"$out" | grep -q '5 older finished jobs not shown.*--all' ||
    { echo "FAIL INV-9: no line counts the rest: $out"; exit 1; }
[ "$(wc -l <<<"$out")" = 22 ] || { echo "FAIL INV-9: $(wc -l <<<"$out") lines, want 22"; exit 1; }
out="$(env CC_JOB_DIR="$many" PATH="$fake:$PATH" "$bin" status --all)"
[ "$(wc -l <<<"$out")" = 26 ] || { echo "FAIL INV-9: --all printed $(wc -l <<<"$out") lines, want 26"; exit 1; }
[ "$(sed -n 1p <<<"$out" | cut -d: -f1)" = job01 ] || { echo "FAIL INV-9: --all is not by name: $out"; exit 1; }
case "$("$bin" --help)" in *"status --all"*) ;; *) echo "FAIL INV-9: --help omits --all"; exit 1 ;; esac
# The state dir may be a symlink to a data drive, as this script's header offers.
ln -s "$many" "$prefix/many-link"
out="$(env CC_JOB_DIR="$prefix/many-link" PATH="$fake:$PATH" "$bin" status)"
[ "$(wc -l <<<"$out")" = 22 ] ||
    { echo "FAIL INV-9: a symlinked state dir listed: $out"; exit 1; }
# INV-10 — with list-units failing, status falls back to asking per job rather
# than calling a running job finished.
rm -f "$listing"
out="$(env CC_JOB_DIR="$many" PATH="$fake:$PATH" "$bin" status)"
sed -n 1p <<<"$out" | grep -q 'oldrun: running' ||
    { echo "FAIL INV-10: a failed list-units hid a running job: $out"; exit 1; }
[ -s "$calls" ] || { echo "FAIL INV-10: no per-job fallback ran"; exit 1; }

# INV-11 — start records the starting session, and status shows it. The probe
# above was started with CLAUDE_CODE_SESSION_ID=session-leak.
grep -qx 'owner=session-leak' "$jobs/probe.info" ||
    { echo "FAIL INV-11: start did not record the owning session"; exit 1; }
out="$(env CC_JOB_DIR="$jobs" PATH="$fake:$PATH" "$bin" status probe)"
case "$out" in
    *"session session-"*) ;;
    *) echo "FAIL INV-11: status does not show the owner: $out"; exit 1 ;;
esac

# INV-12 — clean --mine removes only this session's finished jobs whose output
# was read; --force drops the read condition, never the running one.
mine="$prefix/mine"
mkdir "$mine"
mkjob() { # <name> <owner, or empty> <read|unread>
    echo out >"$mine/$1.log"; echo 0 >"$mine/$1.exit"
    { echo "started=2026-01-01"; if [ -n "$2" ]; then echo "owner=$2"; fi; } >"$mine/$1.info"
    if [ "$3" = read ]; then : >"$mine/$1.read"; fi
}
mkjob done-a A read; mkjob unread-a A unread; mkjob oldrun A read
mkjob theirs B read; mkjob legacy "" read
rm -f "$mine/oldrun.exit"   # oldrun is the job the stand-in systemctl reports running
: >"$listing"
rc=0
env -u CLAUDE_CODE_SESSION_ID CC_JOB_DIR="$mine" PATH="$fake:$PATH" "$bin" clean --mine \
    >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 2 ] || { echo "FAIL INV-12: clean --mine with no session exited $rc, want 2"; exit 1; }
[ -e "$mine/done-a.log" ] || { echo "FAIL INV-12: clean --mine with no session removed a job"; exit 1; }
out="$(env CLAUDE_CODE_SESSION_ID=A CC_JOB_DIR="$mine" PATH="$fake:$PATH" "$bin" clean --mine 2>&1)"
[ ! -e "$mine/done-a.log" ] || { echo "FAIL INV-12: this session's read job was kept: $out"; exit 1; }
for kept in unread-a oldrun theirs legacy; do
    [ -e "$mine/$kept.log" ] || { echo "FAIL INV-12: clean --mine removed '$kept': $out"; exit 1; }
done
for kept in unread-a oldrun; do
    grep -q "kept $kept" <<<"$out" ||
        { echo "FAIL INV-12: clean --mine does not name '$kept' as kept: $out"; exit 1; }
done
env CLAUDE_CODE_SESSION_ID=A CC_JOB_DIR="$mine" PATH="$fake:$PATH" "$bin" clean --mine --force >/dev/null 2>&1
[ ! -e "$mine/unread-a.log" ] || { echo "FAIL INV-12: --force kept an unread job"; exit 1; }
[ -e "$mine/oldrun.log" ] || { echo "FAIL INV-12: --force removed a running job"; exit 1; }
case "$("$bin" --help)" in *"clean --mine"*) ;; *) echo "FAIL INV-12: --help omits clean --mine"; exit 1 ;; esac

echo "PASS: $bin installs, names its needs, logs under XDG_STATE_HOME, refuses watch without Konsole, forwards exported variables, lists status cheaply, records and cleans by owner"
