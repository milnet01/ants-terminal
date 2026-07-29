#!/bin/sh
# obs-submit.sh (ANTS-3726) — populate the OBS checkout from the repo's recipe
# files and commit a new revision.
#
# Repeatable per-release flow. Run from anywhere; paths resolve relative to this
# script. Assumes obs-setup.sh has created the project + package once.
#
# There is deliberately NO `osc service manualrun` here: _service runs obs_scm
# server-side and tar/recompress/set_version at build time, so committing
# _service is enough to trigger a fresh source fetch. Nothing large is uploaded.
#
# NOTE the spec is NOT stored in packaging/obs/. It lives at
# packaging/opensuse/ants-terminal.spec, which is the single source of truth
# also used for plain rpmbuild — copying it here at submit time is what stops an
# OBS-local fork of the spec silently drifting from the repo's.
#
# Override via env: OBS_API, OBS_PROJECT, OBS_PACKAGE, OBS_WORKDIR, OBS_MSG.
set -eu

API="${OBS_API:-https://api.opensuse.org}"
PROJ="${OBS_PROJECT:-home:milnet:ants-terminal}"
PKG="${OBS_PACKAGE:-ants-terminal}"

HERE="$(cd "$(dirname "$0")" && pwd)"      # packaging/obs
ROOT="$(cd "$HERE/../.." && pwd)"          # repo root
WORKDIR="${OBS_WORKDIR:-$ROOT/build-obs}"  # osc checkout lives here (gitignored)
SPEC="$ROOT/packaging/opensuse/ants-terminal.spec"

command -v osc >/dev/null 2>&1 || { echo "obs-submit: osc not installed" >&2; exit 1; }
[ -f "$SPEC" ] || { echo "obs-submit: spec not found: $SPEC" >&2; exit 1; }

# The tag _service pins must exist on GitHub, or obs_scm cannot clone it and the
# build breaks at source fetch rather than at compile — a confusing failure to
# debug from the build log alone. Check it here where the message can be clear.
REV="$(sed -n 's/.*<param name="revision">\(.*\)<\/param>.*/\1/p' "$HERE/_service")"
if [ -n "$REV" ] && command -v git >/dev/null 2>&1; then
    if ! git -C "$ROOT" rev-parse -q --verify "refs/tags/$REV" >/dev/null 2>&1; then
        echo "obs-submit: _service pins tag '$REV', which does not exist locally." >&2
        echo "            Cut/fetch that tag first, or update _service's <revision>." >&2
        exit 1
    fi
fi

mkdir -p "$WORKDIR"
CO="$WORKDIR/$PROJ/$PKG"
if [ -d "$CO/.osc" ]; then
    echo ">>> updating checkout: $CO"
    ( cd "$CO" && osc -A "$API" update )
else
    echo ">>> checking out $PROJ/$PKG"
    ( cd "$WORKDIR" && osc -A "$API" checkout "$PROJ" "$PKG" )
fi

echo ">>> copying recipe files"
cp "$HERE/_service" "$CO/_service"
cp "$SPEC" "$CO/ants-terminal.spec"
[ -f "$HERE/ants-terminal.changes" ] && cp "$HERE/ants-terminal.changes" "$CO/"

cd "$CO"
osc -A "$API" add _service ants-terminal.spec ants-terminal.changes 2>/dev/null || true
osc -A "$API" addremove 2>/dev/null || true
osc -A "$API" commit -m "${OBS_MSG:-ants-terminal $REV}"

echo "OK — committed. Watch it with: packaging/obs/obs-status.sh"
