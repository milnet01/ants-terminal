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

echo "PASS: $bin installs, names its needs, logs under XDG_STATE_HOME, refuses watch without Konsole, forwards exported variables"
