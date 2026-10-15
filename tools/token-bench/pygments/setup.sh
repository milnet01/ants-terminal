#!/usr/bin/env bash
# setup.sh WORKDIR — build one run's copy of the pygments bench (ANTS-5697).
#
# Pygments 2.21.0 at a pinned commit, without tests/contrast (it needs
# wcag_contrast_ratio, which is not installed), with plant.patch applied and
# test_style_colours.py added. The upstream history is dropped so the plant
# cannot be read back out of git; bench.py then commits the result as the
# run's one starting commit. The clone is cached in $BENCH_CACHE.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
WORKDIR=$1
COMMIT=a43b45dcf081b6010c6ab4428f149f7f6d2499c4
CACHE=${BENCH_CACHE:?BENCH_CACHE must name a cache folder}/pygments-2.21.0

if [ ! -d "$CACHE" ]; then
    git -c advice.detachedHead=false clone -q --depth 1 --branch 2.21.0 \
        https://github.com/pygments/pygments "$CACHE"
fi
if [ "$(git -C "$CACHE" rev-parse HEAD)" != "$COMMIT" ]; then
    echo "setup.sh: $CACHE is not at $COMMIT" >&2
    exit 1
fi

git -c advice.detachedHead=false clone -q "$CACHE" "$WORKDIR"
cd "$WORKDIR"
git apply "$HERE/plant.patch"
rm -rf .git tests/contrast
cp "$HERE/test_style_colours.py" tests/
