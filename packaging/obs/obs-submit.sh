#!/bin/sh
# obs-submit.sh (ANTS-3726) — populate the OBS checkout from the repo's recipe
# files and commit a new revision.
#
# Repeatable per-release flow. Run from anywhere; paths resolve relative to this
# script. Assumes obs-setup.sh has created the project + package once.
#
# There is deliberately NO `osc service manualrun` here: _service runs obs_scm
# server-side and tar/recompress at build time, so committing _service is enough
# to trigger a fresh source fetch. Nothing large is uploaded.
#
# NOTE the spec is NOT stored in packaging/obs/. It lives at
# packaging/opensuse/ants-terminal.spec, which is the single source of truth
# also used for plain rpmbuild — copying it here at submit time is what stops an
# OBS-local fork of the spec silently drifting from the repo's.
#
# `--staging <sha> <version>` (ANTS-5305) sends a COMMIT, not a tag, to the
# staging project, so every distro can build it before the tag exists. The
# staging project has publishing disabled: nothing it builds is offered to
# anyone. The recipe it commits is packaging/obs/_service with the revision
# replaced by the commit and the version given literally, since there is no
# tag yet for obs_scm to derive one from.
#
# Override via env: OBS_API, OBS_PROJECT, OBS_PACKAGE, OBS_WORKDIR, OBS_MSG,
# OBS_RETRY_SECS (the pause before a commit is tried again, default 30).
set -eu

STAGING_SHA=""
STAGING_VERSION=""
if [ "${1:-}" = "--staging" ]; then
    STAGING_SHA="${2:-}"
    STAGING_VERSION="${3:-}"
    if [ -z "$STAGING_SHA" ] || [ -z "$STAGING_VERSION" ] || [ $# -ne 3 ]; then
        echo "obs-submit: usage: obs-submit.sh [--staging <sha> <version>]" >&2
        exit 2
    fi
elif [ $# -ne 0 ]; then
    echo "obs-submit: usage: obs-submit.sh [--staging <sha> <version>]" >&2
    exit 2
fi

API="${OBS_API:-https://api.opensuse.org}"
if [ -n "$STAGING_SHA" ]; then
    PROJ="${OBS_PROJECT:-home:milnet:ants-terminal-staging}"
else
    PROJ="${OBS_PROJECT:-home:milnet:ants-terminal}"
fi
PKG="${OBS_PACKAGE:-ants-terminal}"

HERE="$(cd "$(dirname "$0")" && pwd)"      # packaging/obs
ROOT="$(cd "$HERE/../.." && pwd)"          # repo root
WORKDIR="${OBS_WORKDIR:-$ROOT/build-obs}"  # osc checkout lives here (gitignored)
SPEC="$ROOT/packaging/opensuse/ants-terminal.spec"
# rpmlint auto-loads this out of SOURCES in the build VM; it needs no flag, only
# to be committed next to the spec. Lives beside the spec for the same reason
# the spec does — one copy, no OBS-local fork.
LINTRC="$ROOT/packaging/opensuse/ants-terminal-rpmlintrc"
# Patches live beside the spec, under the same rule: one copy, no OBS-local
# fork. Carried as a directory rather than a named list so adding or removing
# one is a repo edit alone.
PATCHDIR="$ROOT/packaging/opensuse"

command -v osc >/dev/null 2>&1 || { echo "obs-submit: osc not installed" >&2; exit 1; }
[ -f "$SPEC" ] || { echo "obs-submit: spec not found: $SPEC" >&2; exit 1; }
[ -f "$LINTRC" ] || { echo "obs-submit: rpmlintrc not found: $LINTRC" >&2; exit 1; }

# The tag _service pins must exist on GitHub, or obs_scm cannot clone it and the
# build breaks at source fetch rather than at compile — a confusing failure to
# debug from the build log alone. Check it here where the message can be clear.
REV="$(sed -n 's/.*<param name="revision">\(.*\)<\/param>.*/\1/p' "$HERE/_service")"
URL="$(sed -n 's/.*<param name="url">\(.*\)<\/param>.*/\1/p' "$HERE/_service")"
if [ -n "$STAGING_SHA" ]; then
    # obs_scm clones from GitHub, so the commit has to be there. origin/main
    # is this checkout's record of the last push or fetch.
    if ! git -C "$ROOT" merge-base --is-ancestor "$STAGING_SHA" origin/main 2>/dev/null; then
        echo "obs-submit: commit '$STAGING_SHA' is not on origin/main." >&2
        echo "            Push it first: OBS clones from $URL" >&2
        exit 1
    fi
elif [ -n "$REV" ] && command -v git >/dev/null 2>&1; then
    if ! git -C "$ROOT" rev-parse -q --verify "refs/tags/$REV" >/dev/null 2>&1; then
        echo "obs-submit: _service pins tag '$REV', which does not exist locally." >&2
        echo "            Cut/fetch that tag first, or update _service's <revision>." >&2
        exit 1
    fi
    # Audit TL-32 — a local tag is not a pushed one, and obs_scm clones from
    # the URL, so ask the remote the service actually fetches from.
    if [ -n "$URL" ] && ! git ls-remote --exit-code --tags "$URL" "refs/tags/$REV" >/dev/null 2>&1; then
        echo "obs-submit: tag '$REV' is not on $URL (or it could not be reached)." >&2
        echo "            Push the tag first: git push origin $REV" >&2
        exit 1
    fi
fi

# Expand the spec for real and syntax-check the scriptlets it generates.
#
# ANTS-4719 — a comment macro in the PREAMBLE, which the scriptlet check below
# cannot see. That one bash -n's each EXPANDED section body, so it catches an
# unescaped macro inside %%build or %%check. In the preamble the expansion
# produces a section marker or a tag instead of broken shell: rpmspec -P still
# succeeds on a host whose rpm only warns, every scriptlet still parses, and the
# failure arrives as "Unknown tag" a minute into a foreign build.
#
# Measured 2026-08-26: a BuildRequires comment reading "runs %%check" (written
# with one %%) built fine here and took out Mageia_10 alone, because this host's
# rpm warns where Mageia's errors. Needs no rpmspec, so unlike the check below
# it still runs where rpmspec is absent.
bad_comment_macro="$(awk '
    /^[[:space:]]*#/ {
        line = $0
        gsub(/%%/, "", line)
        if (line ~ /%[A-Za-z_{]/) printf "%d: %s\n", NR, $0
    }' "$SPEC")"
if [ -n "$bad_comment_macro" ]; then
    echo "obs-submit: unescaped macro reference in a comment:" >&2
    printf '%s\n' "$bad_comment_macro" >&2
    echo "            rpm expands macros in comments — double it (%%check)." >&2
    exit 1
fi
echo ">>> no unescaped macro references in comments"

# rpm expands macros inside `#` comments — they are comments to the shell, not
# to the macro engine. So a singly-written macro reference in a comment (%ctest
# rather than %%ctest) expands mid-comment and can push live text into %build or
# %check, where bash meets it as a syntax error. That failure is invisible until
# ten minutes into an OBS build, and it lands AFTER the whole test suite has run
# green, which makes it read like a test problem. It cost exactly one such cycle
# on 2026-07-29. This check costs about a second.
if command -v rpmspec >/dev/null 2>&1; then
    exp="$(mktemp)"
    trap 'rm -f "$exp"' EXIT
    if ! rpmspec -P "$SPEC" > "$exp" 2>/dev/null; then
        echo "obs-submit: rpmspec could not parse $SPEC" >&2
        exit 1
    fi
    for sect in build install check post postun; do
        body="$(awk -v pat="^%$sect\$" '$0 ~ pat {f=1; next} f && /^%[a-z]/ {exit} f' "$exp")"
        [ -n "$body" ] || continue
        if ! printf '%s\n' "$body" | bash -n 2>/dev/null; then
            echo "obs-submit: %$sect is not valid shell after macro expansion." >&2
            echo "            A macro reference in a comment there likely needs" >&2
            echo "            doubling (write %%${sect}-style refs as %%%%...)." >&2
            exit 1
        fi
    done
    echo ">>> spec expands and its scriptlets parse as shell"
else
    echo ">>> rpmspec not installed — skipping the spec expansion check" >&2
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
if [ -n "$STAGING_SHA" ]; then
    # The same recipe, aimed at a commit: no tag to match and no tag-derived
    # version to rewrite, so those three parameters go and the version is
    # given as written.
    sed -e "s|<param name=\"revision\">[^<]*</param>|<param name=\"revision\">$STAGING_SHA</param>|" \
        -e "s|<param name=\"versionformat\">[^<]*</param>|<param name=\"versionformat\">$STAGING_VERSION</param>|" \
        -e '/<param name="match-tag">/d' \
        -e '/<param name="versionrewrite-/d' \
        "$HERE/_service" > "$CO/_service"
    if ! grep -q "<param name=\"revision\">$STAGING_SHA</param>" "$CO/_service" \
            || ! grep -q "<param name=\"versionformat\">$STAGING_VERSION</param>" "$CO/_service"; then
        echo "obs-submit: could not aim $HERE/_service at commit $STAGING_SHA." >&2
        exit 1
    fi
else
    cp "$HERE/_service" "$CO/_service"
fi
cp "$SPEC" "$CO/ants-terminal.spec"
cp "$LINTRC" "$CO/ants-terminal-rpmlintrc"
[ -f "$HERE/ants-terminal.changes" ] && cp "$HERE/ants-terminal.changes" "$CO/"

# Cleared before copying so a patch DELETED from the repo also disappears from
# the package — `osc addremove` below then drops it. Without the rm, removing a
# backport upstream would leave it applied here forever, which is the drift
# this whole copy-don't-fork arrangement exists to prevent.
rm -f "$CO"/*.patch
for _p in "$PATCHDIR"/*.patch; do
    [ -e "$_p" ] || break
    cp "$_p" "$CO/"
    echo ">>> carrying patch $(basename "$_p")"
done

# ANTS-3731 — stamp Version: from the pinned tag, replacing _service's
# set_version service, which used to do this inside the build VM.
#
# set_version ran at mode="buildtime", which makes obs-service-set_version a
# BUILD DEPENDENCY of every job, in every target repository. openSUSE:Tools
# cannot build that service for some targets at all (for Mageia its own status
# is unresolvable, "nothing provides python3-base"), so a whole job went
# unresolvable over a step that rewrites one line. Computing it here needs no
# service in the target repo, and removes the dependency from every target
# rather than just the one that noticed.
#
# The value must match what obs_scm derives, or Source0 names a tarball the tar
# service never produced: _service's versionrewrite maps `v(.*)` to `\1`, so
# stripping one leading `v` is that same transform.
#
# This is the ONE line of the committed spec that differs from the repo's, and
# it is computed from the tag rather than maintained by hand — so the tag stays
# the single source of truth, exactly as it was under set_version. Every other
# line is copied verbatim, which is what stops an OBS-local fork drifting.
[ -n "$REV" ] || {
    echo "obs-submit: _service has no <revision>, so Version: cannot be stamped." >&2
    exit 1
}
if [ -n "$STAGING_SHA" ]; then
    VERSION="$STAGING_VERSION"
    REV="$STAGING_SHA as $STAGING_VERSION"
else
    VERSION="${REV#v}"
fi
case "$VERSION" in
    *-*)
        echo "obs-submit: tag '$REV' gives RPM Version '$VERSION', which contains a" >&2
        echo "            '-' and is not a legal rpm version. OBS tracks release" >&2
        echo "            tags only (vX.Y.Z)." >&2
        exit 1 ;;
esac
sed -i "s/^Version:[[:space:]].*/Version:        $VERSION/" "$CO/ants-terminal.spec"
# Verify rather than trust: a sed that matched nothing is silent, and the
# resulting build fails much later at source fetch with a 404 on Source0.
grep -qx "Version:        $VERSION" "$CO/ants-terminal.spec" || {
    echo "obs-submit: could not stamp Version: $VERSION into the copied spec." >&2
    echo "            Does $SPEC still have a 'Version:' line?" >&2
    exit 1
}
echo ">>> stamped Version: $VERSION (from $REV)"

cd "$CO"
osc -A "$API" add _service ants-terminal.spec ants-terminal-rpmlintrc ants-terminal.changes 2>/dev/null || true
osc -A "$API" addremove 2>/dev/null || true

# osc exits non-zero here for two reasons that both clear on a second try.
#
# Pushing a release tag fires .obs/workflows.yml's trigger_services, which adds
# a package revision of its own. When that lands between the update above and
# this commit, osc refuses the commit as out of date (it cost the 0.7.112
# submit, 2026-09-30). The trigger does not touch the files committed here.
#
# And after a commit lands, osc waits for the server-side service run; a
# server error in that wait (HTTP 503, seen the same day) fails the command
# with the revision already committed. The next try then finds nothing to
# commit and exits 0.
tries=0
until osc -A "$API" commit -m "${OBS_MSG:-ants-terminal $REV}"; do
    tries=$((tries + 1))
    if [ "$tries" -ge 5 ]; then
        echo "obs-submit: the commit did not complete in $tries tries — giving up." >&2
        echo "            Run this script again once OBS answers; it is safe to repeat." >&2
        exit 1
    fi
    echo ">>> the commit did not complete; updating the checkout and trying again ($tries)" >&2
    sleep "${OBS_RETRY_SECS:-30}"
    osc -A "$API" update || true
done

echo "OK — committed. Wait for the builds with: packaging/obs/obs-status.sh"
