#!/usr/bin/env bash
#
# release-notes.sh <X.Y.Z> — print the GitHub release text for a version.
#
# The text is the plain-language lead of that version's CHANGELOG section:
# everything between "## [X.Y.Z]" and its first "### " category, with the
# "**Theme:** " label removed. A link to the full change list follows.
#
# release.yml runs this when it creates the release. The release body is also
# what the project website shows as its changelog, which is why it is the lead
# and not the category entries: those are written for developers and can run
# past GitHub's size limit for a release body.
#
# Reads CHANGELOG.md in the current directory. Exits 1 when the version has no
# section there.

set -euo pipefail

ver=${1:-}
[ -n "$ver" ] || { echo "release-notes: usage: release-notes.sh <X.Y.Z>" >&2; exit 2; }
[ -f CHANGELOG.md ] || { echo "release-notes: no CHANGELOG.md here" >&2; exit 1; }

# awk program, single-quoted deliberately: $0 is awk's own field.
# shellcheck disable=SC2016
awk -v ver="$ver" '
    index($0, "## [" ver "]") == 1 { found = 1; inlead = 1; next }
    inlead && (/^## \[/ || /^### /) { inlead = 0 }
    inlead {
        sub(/^\*\*Theme:\*\*[ \t]*/, "")
        # Hold blank lines until more text follows, so the lead carries no
        # blank line before or after it.
        if ($0 ~ /^[ \t]*$/) { if (printed) held = held "\n"; next }
        printf "%s%s\n", held, $0
        held = ""; printed = 1
    }
    END { exit (found ? 0 : 1) }
' CHANGELOG.md || { echo "release-notes: CHANGELOG.md has no '## [${ver}]' section" >&2; exit 1; }

printf '\nFull change list: [CHANGELOG.md at this tag](https://github.com/milnet01/ants-terminal/blob/v%s/CHANGELOG.md)\n' "$ver"
