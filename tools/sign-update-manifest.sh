#!/usr/bin/env bash
# ANTS-5560 — writes and signs the self-update manifest for one AppImage
# (docs/specs/ANTS-5560-appimage-self-update.md § 2.3).
#
# Usage: tools/sign-update-manifest.sh <appimage> <version> [<public-key.pem>]
#
# Writes <appimage>.manifest (compact JSON) and <appimage>.manifest.sig (the
# raw 64-byte Ed25519 signature over the manifest's exact bytes), then
# verifies the signature against the public key — the committed
# packaging/update-signing/ants-update.pub.pem unless a third argument names
# another. The private key PEM comes from $ANTS_UPDATE_SIGNING_KEY. Any
# failure exits non-zero and leaves neither file behind: a release never
# ships unsigned.
set -euo pipefail

die() { echo "sign-update-manifest.sh: $*" >&2; exit 1; }

[[ $# -ge 2 && $# -le 3 ]] || die "usage: $0 <appimage> <version> [<public-key.pem>]"
appimage=$1
version=$2
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
pubkey=${3:-$here/../packaging/update-signing/ants-update.pub.pem}

[[ -n ${ANTS_UPDATE_SIGNING_KEY:-} ]] || die "ANTS_UPDATE_SIGNING_KEY is not set"
[[ -f $appimage ]] || die "no such AppImage: $appimage"
[[ -f $pubkey ]] || die "no such public key: $pubkey"
[[ $version =~ ^[0-9A-Za-z.+-]+$ ]] || die "bad version: $version"

asset=$(basename -- "$appimage")
[[ $asset == "Ants_Terminal-${version}-x86_64.AppImage" ]] \
    || die "asset $asset is not Ants_Terminal-${version}-x86_64.AppImage"

manifest=$appimage.manifest
sig=$manifest.sig
work=$(mktemp -d)
cleanup() { rm -rf -- "$work"; }
trap cleanup EXIT
fail() { rm -f -- "$manifest" "$sig"; die "$@"; }

(umask 077; printf '%s\n' "$ANTS_UPDATE_SIGNING_KEY" > "$work/key.pem")

size=$(stat -c %s -- "$appimage")
sha=$(sha256sum -- "$appimage" | cut -d' ' -f1)
printf '{"format":1,"version":"%s","asset":"%s","size":%s,"sha256":"%s"}' \
    "$version" "$asset" "$size" "$sha" > "$manifest"

openssl pkeyutl -sign -inkey "$work/key.pem" -rawin -in "$manifest" -out "$sig" \
    || fail "signing failed"
[[ $(stat -c %s -- "$sig") -eq 64 ]] || fail "signature is not 64 bytes"
openssl pkeyutl -verify -pubin -inkey "$pubkey" -rawin -in "$manifest" -sigfile "$sig" >/dev/null \
    || fail "signature does not verify against $pubkey"

echo "signed $asset ($size bytes)"
