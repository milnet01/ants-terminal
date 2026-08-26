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
# Override via env: OBS_API, OBS_PROJECT, OBS_PACKAGE, OBS_WORKDIR, OBS_MSG.
set -eu

API="${OBS_API:-https://api.opensuse.org}"
PROJ="${OBS_PROJECT:-home:milnet:ants-terminal}"
PKG="${OBS_PACKAGE:-ants-terminal}"

HERE="$(cd "$(dirname "$0")" && pwd)"      # packaging/obs
ROOT="$(cd "$HERE/../.." && pwd)"          # repo root
WORKDIR="${OBS_WORKDIR:-$ROOT/build-obs}"  # osc checkout lives here (gitignored)
SPEC="$ROOT/packaging/opensuse/ants-terminal.spec"
# rpmlint auto-loads this out of SOURCES in the build VM; it needs no flag, only
# to be committed next to the spec. Lives beside the spec for the same reason
# the spec does — one copy, no OBS-local fork.
LINTRC="$ROOT/packaging/opensuse/ants-terminal-rpmlintrc"

command -v osc >/dev/null 2>&1 || { echo "obs-submit: osc not installed" >&2; exit 1; }
[ -f "$SPEC" ] || { echo "obs-submit: spec not found: $SPEC" >&2; exit 1; }
[ -f "$LINTRC" ] || { echo "obs-submit: rpmlintrc not found: $LINTRC" >&2; exit 1; }

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
        gsub(/%%%%/, "", line)
        if (line ~ /%%[A-Za-z_{]/) printf "%%d: %%s\n", NR, $0
    }' "$SPEC")"
if [ -n "$bad_comment_macro" ]; then
    echo "obs-submit: unescaped macro reference in a comment:" >&2
    printf '%%s\n' "$bad_comment_macro" >&2
    echo "            rpm expands macros in comments — double it (%%%%check)." >&2
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
cp "$HERE/_service" "$CO/_service"
cp "$SPEC" "$CO/ants-terminal.spec"
cp "$LINTRC" "$CO/ants-terminal-rpmlintrc"
[ -f "$HERE/ants-terminal.changes" ] && cp "$HERE/ants-terminal.changes" "$CO/"

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
VERSION="${REV#v}"
case "$VERSION" in
    *-*)
        echo "obs-submit: tag '$REV' gives RPM Version '$VERSION', which contains a" >&2
        echo "            '-' and is not a legal rpm version. OBS tracks promoted" >&2
        echo "            release tags only — pin one of those, not an RC tag." >&2
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
echo ">>> stamped Version: $VERSION (from tag $REV)"

cd "$CO"
osc -A "$API" add _service ants-terminal.spec ants-terminal-rpmlintrc ants-terminal.changes 2>/dev/null || true
osc -A "$API" addremove 2>/dev/null || true
osc -A "$API" commit -m "${OBS_MSG:-ants-terminal $REV}"

echo "OK — committed. Watch it with: packaging/obs/obs-status.sh"
