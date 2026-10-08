#!/usr/bin/env bash
# ANTS-5629 — install the cc-job component into a scratch prefix and run the
# installed copy. No systemd --user is needed: nothing here starts a job.
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

echo "PASS: $bin installs, names its needs, logs under XDG_STATE_HOME, refuses watch without Konsole"
