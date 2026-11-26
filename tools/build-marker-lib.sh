# shellcheck shell=bash
# ANTS-6595 — an interrupted-build marker, shared by tools/local-ci.sh and
# tools/asan-nightly.sh. Sourced, not run. tools/qt62-guard.sh keeps its own
# copy of the same rule, because its trap must also stop a podman container.
#
# marked_build <marker> <who> <command...>
#
# The marker is written before the command starts and removed only when the
# command returns its own result, success or failure. So a kill that no trap
# sees (SIGKILL, an OOM kill) still leaves the tree marked untrusted, and the
# next run rebuilds it cold rather than trusting an incremental build over a
# killed ninja.
#
# The command runs in the foreground, not backgrounded and waited on. A job a
# script backgrounds ignores SIGINT, so Ctrl-C would leave the build running
# after the script exits. In the foreground a terminal's Ctrl-C and a
# timeout's signal reach the build's whole process group, and the trap below
# runs once the build has stopped. The trap only reports and exits 143; the
# marker is already on disk.
#
# Returns the command's status. An exit of 125 or more is not the command's
# own result (128+n is a signal), so the marker stays.
marked_build() {
    local marker=$1 who=$2
    shift 2
    : > "$marker" || { echo "$who: cannot write $marker" >&2; return 1; }
    trap 'echo >&2
          echo "$who: interrupted — $(dirname "$marker") stays marked untrusted," >&2
          echo "          so the next run will not trust an incremental build." >&2
          exit 143' TERM INT HUP
    "$@"
    local rc=$?
    trap - TERM INT HUP
    (( rc < 125 )) && rm -f "$marker"
    return "$rc"
}
