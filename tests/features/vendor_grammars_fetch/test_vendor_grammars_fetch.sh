#!/bin/bash
# ANTS-6631 — tools/vendor-grammars.sh keeps the vendored runtime when its
# download fails. See spec.md.
#
# Runs a copy of the real script in a throwaway tree, with curl stubbed on
# PATH, so nothing is fetched and the repo's third_party/ is never touched.

set -uo pipefail

SCRIPT="$(cd "$(dirname "$0")/../../.." && pwd)/tools/vendor-grammars.sh"
for tool in jq sha256sum tar; do
    command -v "$tool" >/dev/null 2>&1 || { echo "SKIP: $tool not available"; exit 77; }
done

failures=0
check() {  # check <description> <condition-result>
    if [[ "$2" == "0" ]]; then
        echo "  ok   — $1"
    else
        echo "  FAIL — $1"
        failures=$((failures + 1))
    fi
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The stub curl copies STUB_TARBALL to its -o file, or fails as a 404 does
# when STUB_TARBALL is unset.
mkdir -p "$tmp/bin"
cat > "$tmp/bin/curl" <<'EOF'
#!/bin/bash
[[ -n "${STUB_TARBALL:-}" ]] || exit 22
while [[ $# -gt 0 ]]; do
    [[ "$1" == -o ]] && { cp "$STUB_TARBALL" "$2"; exit 0; }
    shift
done
exit 2
EOF
chmod +x "$tmp/bin/curl"

tree="$tmp/tree"
manifest="$tree/third_party/tree-sitter-grammars/MANIFEST.json"
setup() {  # a fresh tree: the script, an unpinned runtime entry, an old runtime
    rm -rf "$tree"
    mkdir -p "$tree/tools" "$tree/third_party/tree-sitter-grammars" \
             "$tree/third_party/tree-sitter"
    cp "$SCRIPT" "$tree/tools/"
    echo '{"runtime": {"repo": "https://github.com/x/tree-sitter", "commit": "abc",
           "sha256": ""}, "grammars": []}' > "$manifest"
    echo old > "$tree/third_party/tree-sitter/kept.txt"
}
run() {  # sets $out and $rc
    out=$(PATH="$tmp/bin:$PATH" TMPDIR="$tmp" \
          bash "$tree/tools/vendor-grammars.sh" --pin tree-sitter 2>&1)
    rc=$?
}

echo "a failed download under --pin leaves the vendored runtime alone"
setup
run
check "exit non-zero (got $rc)" "$([[ $rc -ne 0 ]] && echo 0 || echo 1)"
check "the old runtime is still there" \
      "$([[ -f "$tree/third_party/tree-sitter/kept.txt" ]] && echo 0 || echo 1)"
check "no hash was pinned" \
      "$([[ "$(jq -r .runtime.sha256 "$manifest")" == "" ]] && echo 0 || echo 1)"

echo "control: a good download replaces the runtime and pins its hash"
mkdir -p "$tmp/src/ts-abc/lib/src" "$tmp/src/ts-abc/lib/include"
echo new > "$tmp/src/ts-abc/lib/src/lib.c"
echo lic > "$tmp/src/ts-abc/LICENSE"
tar -czf "$tmp/ts.tar.gz" -C "$tmp/src" ts-abc
setup
STUB_TARBALL="$tmp/ts.tar.gz" run
check "exit 0 (got $rc)" "$([[ $rc -eq 0 ]] && echo 0 || echo 1)"
check "the new runtime is in place" \
      "$([[ -f "$tree/third_party/tree-sitter/lib/src/lib.c" ]] && echo 0 || echo 1)"
check "the tarball's hash is pinned" \
      "$([[ "$(jq -r .runtime.sha256 "$manifest")" == "$(sha256sum "$tmp/ts.tar.gz" | cut -d' ' -f1)" ]] \
         && echo 0 || echo 1)"

if [[ $failures -gt 0 ]]; then
    echo "FAILED: $failures assertion(s)"
    echo "$out"
    exit 1
fi
echo "PASS"
exit 0
