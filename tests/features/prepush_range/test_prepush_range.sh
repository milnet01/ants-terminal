#!/usr/bin/env bash
# tests/features/prepush_range/test_prepush_range.sh — see spec.md.
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
if ! python3 -c 'import yaml' 2>/dev/null; then
    echo "[skip] python3 + PyYAML not available"; exit 0
fi
failures=0
fail() { failures=$((failures + 1)); printf '[FAIL] %s\n' "$*" >&2; }
pass() { printf '[ ok ] %s\n' "$*"; }

T="$(mktemp -d -t ants-prepush-range.XXXXXX)"
trap 'rm -rf "$T"' EXIT
git -C "$T" init -q -b main
git -C "$T" config user.email t@t; git -C "$T" config user.name t
mkdir -p "$T/tools/hooks" "$T/.github/workflows" "$T/docs"
cp "$REPO_ROOT/tools/hooks/pre-push" "$T/tools/hooks/pre-push"
cp "$REPO_ROOT/.github/workflows/ci.yml" "$T/.github/workflows/ci.yml"
cat > "$T/tools/ci_workflow.py" <<PY
#!/usr/bin/env python3
import subprocess, sys
if len(sys.argv) > 1 and sys.argv[1] == "docs-only":
    sys.exit(subprocess.call([sys.executable, "$REPO_ROOT/tools/ci_workflow.py"] + sys.argv[1:],
                             cwd="$T"))
print("GATE-RAN")
sys.exit(0)
PY
printf 'x = 1\n' > "$T/tools/x.py"
printf '# a\n' > "$T/docs/a.md"
git -C "$T" add -A; git -C "$T" commit -qm base
BASE="$(git -C "$T" rev-parse HEAD)"

run_hook() {  # stdin lines -> combined output
    (cd "$T" && ANTS_PREPUSH_NO_ASAN=1 ANTS_PREPUSH_NO_QT62=1 ANTS_PREPUSH_NO_UBUNTU24=1 \
        bash tools/hooks/pre-push origin url 2>&1)
}

# INV-1 — a rename of a code file into docs/.
git -C "$T" mv tools/x.py docs/x.md; git -C "$T" commit -qm rename
REN="$(git -C "$T" rev-parse HEAD)"
out="$(printf 'refs/heads/main %s refs/heads/main %s\n' "$REN" "$BASE" | run_hook)"
if grep -q 'GATE-RAN' <<<"$out" && ! grep -q 'docs-only set' <<<"$out"; then
    pass "INV-1 a rename out of code runs the gate"
else
    fail "INV-1 a rename out of code skipped the gate: $(grep -m1 'docs-only\|GATE' <<<"$out")"
fi

# INV-2 — one ref's range cannot be diffed; another ref is docs-only.
git -C "$T" checkout -q -b side "$BASE"
printf '# b\n' > "$T/docs/b.md"; git -C "$T" add -A; git -C "$T" commit -qm docs
DOC="$(git -C "$T" rev-parse HEAD)"
UNKNOWN="1234567890abcdef1234567890abcdef12345678"
out="$(printf 'refs/heads/main %s refs/heads/main %s\nrefs/heads/side %s refs/heads/side %s\n' \
        "$REN" "$UNKNOWN" "$DOC" "$BASE" | run_hook)"
if grep -q 'GATE-RAN' <<<"$out" && ! grep -q 'docs-only set' <<<"$out"; then
    pass "INV-2 an undiffable range runs the gate"
else
    fail "INV-2 an undiffable range let a docs-only ref skip the gate: $(grep -m1 'docs-only\|GATE' <<<"$out")"
fi

if [ "$failures" -gt 0 ]; then printf '%d assertion(s) failed.\n' "$failures" >&2; exit "$failures"; fi
echo "all prepush-range assertions passed"
