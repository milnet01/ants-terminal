#!/usr/bin/env bash
# tests/features/prepush_range/test_prepush_range.sh — see spec.md.
set -u
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_COMMON_DIR
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
GLOBAL_HOOKS="${ANTS_GLOBAL_HOOKS:-$HOME/.claude/githooks}"
if ! python3 -c 'import yaml' 2>/dev/null; then
    echo "[skip] python3 + PyYAML not available"; exit 77
fi
have_global=1
[[ -x "$GLOBAL_HOOKS/pre-push" ]] || have_global=0
failures=0
fail() { failures=$((failures + 1)); printf '[FAIL] %s\n' "$*" >&2; }
pass() { printf '[ ok ] %s\n' "$*"; }

T="$(mktemp -d -t ants-prepush-range.XXXXXX)"
WT="$(mktemp -d -t ants-prepush-range-wt.XXXXXX)"
trap 'rm -rf "$T" "$WT"' EXIT
git -C "$T" init -q -b main
git -C "$T" config user.email t@t; git -C "$T" config user.name t
mkdir -p "$T/tools/hooks" "$T/.github/workflows" "$T/docs" "$T/.ants"
cp "$REPO_ROOT/tools/hooks/pre-push" "$T/tools/hooks/pre-push"
cp "$REPO_ROOT/.ants/gate.conf" "$T/.ants/gate.conf"
cp "$REPO_ROOT/.github/workflows/ci.yml" "$T/.github/workflows/ci.yml"
# The real classifier, reached through the path gate.conf configures.
cat > "$T/tools/ci_workflow.py" <<PY
#!/usr/bin/env python3
import subprocess, sys
sys.exit(subprocess.call([sys.executable, "$REPO_ROOT/tools/ci_workflow.py"] + sys.argv[1:]))
PY
# A stand-in gate: it reports how it was called instead of building. It
# declares a --docs mode as the real one does: the machine-wide hook reads the
# gate script for that flag and runs a docs-only push in full without it.
cat > "$T/tools/local-ci.sh" <<'SH'
#!/usr/bin/env bash
case "${1:-}" in
    --docs) mode=--docs ;;
    *)      mode=${1:-full} ;;
esac
echo "GATE-RAN mode=$mode changed=${ANTS_PUSH_CHANGED-<unset>}" | tr '\n' ' '
echo
exit "${STUB_GATE_RC:-0}"
SH
chmod +x "$T/tools/local-ci.sh" "$T/tools/hooks/pre-push"
printf 'x = 1\n' > "$T/tools/x.py"
printf '# a\n' > "$T/docs/a.md"
git -C "$T" add -A; git -C "$T" commit -qm base
BASE="$(git -C "$T" rev-parse HEAD)"

run_hook() {  # stdin lines -> combined output
    rm -rf "$T/.git/ants-gate-passed"
    (cd "$T" && ANTS_GLOBAL_HOOKS="$GLOBAL_HOOKS" bash tools/hooks/pre-push origin url 2>&1)
}

# INV-5 — with no machine-wide hook, the shim runs the project's gate in
# full and its exit status decides the push. Needs no machine-wide hook.
NOHOOKS="$(mktemp -d -t ants-prepush-nohooks.XXXXXX)"
out="$(cd "$T" && ANTS_GLOBAL_HOOKS="$NOHOOKS" bash tools/hooks/pre-push origin url </dev/null 2>&1)"; rc=$?
out_red="$(cd "$T" && STUB_GATE_RC=1 ANTS_GLOBAL_HOOKS="$NOHOOKS" bash tools/hooks/pre-push origin url </dev/null 2>&1)"; rc_red=$?
rmdir "$NOHOOKS"
if grep -q 'GATE-RAN mode=full changed=<unset>' <<<"$out" && [[ $rc -eq 0 ]] \
   && grep -q 'GATE-RAN' <<<"$out_red" && [[ $rc_red -ne 0 ]]; then
    pass "INV-5 with no machine-wide hook the full gate runs and decides the push"
else
    fail "INV-5 no-hook fallback: rc=$rc rc_red=$rc_red: $(grep -m2 'GATE\|pre-push' <<<"$out")"
fi

if [[ "$have_global" -eq 0 ]]; then
    echo "[skip] INV-1..4: no machine-wide hook at $GLOBAL_HOOKS/pre-push"
    if [ "$failures" -gt 0 ]; then exit "$failures"; fi
    exit 0
fi

# INV-4 — a clone with no ants.gate keys of its own takes the committed
# .ants/gate.conf: a docs-only push reaches the gate as --docs, in place.
git -C "$T" checkout -q -b side
printf '# b\n' > "$T/docs/b.md"; git -C "$T" add -A; git -C "$T" commit -qm docs
DOC="$(git -C "$T" rev-parse HEAD)"
local_keys="$(git -C "$T" config --local --get-regexp '^ants\.gate\.' || true)"
out="$(printf 'refs/heads/side %s refs/heads/side %s\n' "$DOC" "$BASE" | run_hook)"
if [[ -z "$local_keys" ]] && grep -q 'in place' <<<"$out" \
   && grep -q 'GATE-RAN mode=--docs changed=docs/b.md' <<<"$out"; then
    pass "INV-4 the committed gate.conf governs a clone with no settings of its own"
else
    fail "INV-4 gate.conf did not govern: $(grep 'GATE\|pre-push' <<<"$out" | head -3)"
fi

# The later cases push tips that are not HEAD: take a fresh checkout for
# them rather than gate.conf's refusal. A clone's own key wins over the file.
git -C "$T" checkout -q main
git -C "$T" config ants.gate.dirtyTree worktree
git -C "$T" config ants.gate.worktreeDir "$WT"

# INV-1 — a rename of a code file into docs/.
git -C "$T" mv tools/x.py docs/x.md; git -C "$T" commit -qm rename
REN="$(git -C "$T" rev-parse HEAD)"
out="$(printf 'refs/heads/main %s refs/heads/main %s\n' "$REN" "$BASE" | run_hook)"
if grep -q 'GATE-RAN mode=full' <<<"$out"; then
    pass "INV-1 a rename out of code runs the full gate"
else
    fail "INV-1 a rename out of code did not run the full gate: $(grep -m1 'GATE\|pre-push' <<<"$out")"
fi

# INV-2 — one ref's range cannot be diffed; another ref is docs-only.
UNKNOWN="1234567890abcdef1234567890abcdef12345678"
out="$(printf 'refs/heads/main %s refs/heads/main %s\nrefs/heads/side %s refs/heads/side %s\n' \
        "$REN" "$UNKNOWN" "$DOC" "$BASE" | run_hook)"
if grep -q 'GATE-RAN mode=full changed=<unset>' <<<"$out" && ! grep -q 'mode=--docs' <<<"$out"; then
    pass "INV-2 an undiffable range runs the full gate with the change set unknown"
else
    fail "INV-2 an undiffable range let a docs-only ref decide: $(grep 'GATE\|pre-push' <<<"$out" | head -3)"
fi

# INV-3 — a documentation-only push reaches the gate as --docs, with its paths.
out="$(printf 'refs/heads/side %s refs/heads/side %s\n' "$DOC" "$BASE" | run_hook)"
if grep -q 'GATE-RAN mode=--docs changed=docs/b.md' <<<"$out"; then
    pass "INV-3 a docs-only push runs the gate's --docs mode"
else
    fail "INV-3 a docs-only push did not run --docs: $(grep 'GATE\|pre-push' <<<"$out" | head -3)"
fi

if [ "$failures" -gt 0 ]; then printf '%d assertion(s) failed.\n' "$failures" >&2; exit "$failures"; fi
echo "all prepush-range assertions passed"
