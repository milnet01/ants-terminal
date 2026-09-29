#!/usr/bin/env bash
# ANTS-5321 — the AppImage launcher. `Ants.AppImage --mcpd` must start the
# bundled ants-mcpd, and every other invocation must start ants-terminal
# with its arguments untouched. Runs packaging/appimage/AppRun against a
# fake AppDir whose two "binaries" print what they were called with.
#
# ANTS-5574 — before starting ants-terminal, AppRun puts a directory first on
# LD_LIBRARY_PATH whose libssl.so / libcrypto.so link to the host's OpenSSL 3,
# and names it in ANTS_OPENSSL3_SHIM; any LD_LIBRARY_PATH already set follows
# it. It does neither for --mcpd, without a private XDG_RUNTIME_DIR, or when no
# OpenSSL 3 is found.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
apprun="$here/../../../packaging/appimage/AppRun"
[ -f "$apprun" ] || { echo "FAIL: $apprun is missing"; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/usr/bin"
for b in ants-terminal ants-mcpd; do
    printf '#!/bin/sh\necho "%s $*"\nprintf "%%s|%%s\\n" "${LD_LIBRARY_PATH:-}" "${ANTS_OPENSSL3_SHIM:-}" > "%s/env.%s"\n' \
        "$b" "$tmp" "$b" > "$tmp/usr/bin/$b"
    chmod +x "$tmp/usr/bin/$b"
done
cp "$apprun" "$tmp/AppRun"
chmod +x "$tmp/AppRun"
# Never touch the real runtime dir; the fake OpenSSL 3 is two empty files.
mkdir -m 700 "$tmp/rt"; export XDG_RUNTIME_DIR="$tmp/rt"
mkdir "$tmp/ssl3" "$tmp/nossl"
touch "$tmp/ssl3/libssl.so.3" "$tmp/ssl3/libcrypto.so.3"
export ANTS_OPENSSL3_DIRS="$tmp/nossl $tmp/ssl3"
unset LD_LIBRARY_PATH ANTS_OPENSSL3_SHIM

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

# ANTS-5574 — the OpenSSL 3 shim.
shim="$tmp/rt/ants-terminal-openssl3"
env_is() {   # env_is <binary> <expected LD_LIBRARY_PATH|ANTS_OPENSSL3_SHIM> <label>
    local got; got="$(cat "$tmp/env.$1")"
    [ "$got" = "$2" ] || { echo "FAIL: $3: env '$got', want '$2'"; fail=1; }
}
"$tmp/AppRun" >/dev/null
env_is ants-terminal "$shim|$shim" "shim first"
for lib in libssl libcrypto; do
    [ "$(readlink "$shim/$lib.so")" = "$tmp/ssl3/$lib.so.3" ] ||
        { echo "FAIL: $lib.so does not link to $tmp/ssl3/$lib.so.3"; fail=1; }
done
[ "$(stat -c %a "$shim" 2>/dev/null)" = 700 ] || { echo "FAIL: shim dir is not 0700"; fail=1; }
LD_LIBRARY_PATH=/opt/x "$tmp/AppRun" >/dev/null
env_is ants-terminal "$shim:/opt/x|$shim" "existing path kept after the shim"
"$tmp/AppRun" --mcpd >/dev/null
env_is ants-mcpd "|" "--mcpd gets no shim"
env -u XDG_RUNTIME_DIR "$tmp/AppRun" >/dev/null
env_is ants-terminal "|" "no XDG_RUNTIME_DIR, no shim"
ANTS_OPENSSL3_DIRS="$tmp/nossl" "$tmp/AppRun" >/dev/null
env_is ants-terminal "|" "no OpenSSL 3, no shim"

[ "$fail" -eq 0 ] && echo "PASS: appimage AppRun dispatch"
exit "$fail"
