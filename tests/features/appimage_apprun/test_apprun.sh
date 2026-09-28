#!/usr/bin/env bash
# ANTS-5321 — the AppImage launcher. `Ants.AppImage --mcpd` must start the
# bundled ants-mcpd, and every other invocation must start ants-terminal
# with its arguments untouched. Runs packaging/appimage/AppRun against a
# fake AppDir whose two "binaries" print what they were called with.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
apprun="$here/../../../packaging/appimage/AppRun"
[ -f "$apprun" ] || { echo "FAIL: $apprun is missing"; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/usr/bin"
for b in ants-terminal ants-mcpd; do
    printf '#!/bin/sh\necho "%s $*"\n' "$b" > "$tmp/usr/bin/$b"
    chmod +x "$tmp/usr/bin/$b"
done
cp "$apprun" "$tmp/AppRun"
chmod +x "$tmp/AppRun"

fail=0
check() {   # check <expected> <args...>
    local want="$1"; shift
    local got
    got="$("$tmp/AppRun" "$@")"
    if [ "$got" != "$want" ]; then
        echo "FAIL: AppRun $* -> '$got', want '$want'"; fail=1
    fi
}
check "ants-mcpd "                      --mcpd
check "ants-mcpd --version"             --mcpd --version
check "ants-terminal "
check "ants-terminal --version"         --version
check "ants-terminal -e --mcpd"         -e --mcpd
# A symlink to AppRun, as the AppImage runtime may invoke it, still finds
# the AppDir.
ln -s "$tmp/AppRun" "$tmp/link"
got="$("$tmp/link" --mcpd)"
[ "$got" = "ants-mcpd " ] || { echo "FAIL: via symlink -> '$got'"; fail=1; }

[ "$fail" -eq 0 ] && echo "PASS: appimage AppRun dispatch"
exit "$fail"
