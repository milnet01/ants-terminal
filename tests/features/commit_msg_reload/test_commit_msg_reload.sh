#!/usr/bin/env bash
# tests/features/commit_msg_reload/test_commit_msg_reload.sh — see spec.md.
set -u
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_COMMON_DIR
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
failures=0
fail() { failures=$((failures + 1)); printf '[FAIL] %s\n' "$*" >&2; }
pass() { printf '[ ok ] %s\n' "$*"; }

T="$(mktemp -d -t ants-commit-msg-reload.XXXXXX)"
trap 'rm -rf "$T"' EXIT
git -C "$T" init -q -b main
git -C "$T" config user.email t@t; git -C "$T" config user.name t
git -C "$T" config core.hooksPath tools/hooks
mkdir -p "$T/tools/hooks" "$T/src/sub" "$T/docs"
if [[ -f "$REPO_ROOT/tools/hooks/commit-msg" ]]; then
    cp "$REPO_ROOT/tools/hooks/commit-msg" "$T/tools/hooks/commit-msg"
    chmod +x "$T/tools/hooks/commit-msg"
fi
printf 'int a;\n' > "$T/src/existing.cpp"
git -C "$T" add -A; git -C "$T" commit -q --no-verify -m base

# try <expect: accept|refuse> <label> <message> -- files to add or change
# Stages the given work, commits with the message, and checks both the exit
# status and whether HEAD moved. Leaves the tree clean either way.
try() {
    local expect="$1" label="$2" msg="$3"
    local before after out rc
    before="$(git -C "$T" rev-parse HEAD)"
    out="$(git -C "$T" commit -q -m "$msg" 2>&1)"; rc=$?
    after="$(git -C "$T" rev-parse HEAD)"
    if [[ "$expect" == accept ]]; then
        if [[ $rc -eq 0 && "$before" != "$after" ]]; then pass "$label"
        else fail "$label — expected the commit to land (rc=$rc): $out"; fi
    else
        if [[ $rc -ne 0 && "$before" == "$after" ]]; then pass "$label"
        else fail "$label — expected a refusal (rc=$rc, HEAD moved: $([[ "$before" != "$after" ]] && echo yes || echo no))"; fi
    fi
    LAST_OUT="$out"
    git -C "$T" reset -q --hard HEAD
    git -C "$T" clean -qfd
}

# INV-1 — an added source file under src/, no Reload: line, is refused and
# the refusal names the file. One case per extension and one in a subfolder.
for f in src/new.cpp src/new.h src/new.c src/new.cc src/new.hpp src/sub/deep.cpp; do
    mkdir -p "$T/$(dirname "$f")"; printf 'int b;\n' > "$T/$f"; git -C "$T" add "$f"
    try refuse "INV-1 adding $f without Reload: is refused" "add $f"
    if [[ "$LAST_OUT" == *"$f"* ]]; then pass "INV-1 the refusal names $f"
    else fail "INV-1 the refusal does not name $f: $LAST_OUT"; fi
done

# INV-2 — a Reload: line with text is accepted; a bare one is refused.
printf 'int c;\n' > "$T/src/ok.cpp"; git -C "$T" add src/ok.cpp
try accept "INV-2 a Reload: line with text is accepted" \
    "add ok

Reload: nothing; pure function."
printf 'int d;\n' > "$T/src/bare.cpp"; git -C "$T" add src/bare.cpp
try refuse "INV-2 a bare Reload: is refused" "add bare

Reload:"

# INV-3 — modifying or deleting an existing src/ file needs no line.
printf 'int e;\n' >> "$T/src/existing.cpp"; git -C "$T" add src/existing.cpp
try accept "INV-3 modifying a src/ file needs no Reload: line" "touch existing"
git -C "$T" rm -q src/existing.cpp
try accept "INV-3 deleting a src/ file needs no Reload: line" "drop existing"

# INV-4 — files outside src/, or non-source files under it, need no line.
mkdir -p "$T/docs"; printf '# x\n' > "$T/docs/x.md"; printf 'x\n' > "$T/src/data.json"
git -C "$T" add docs/x.md src/data.json
try accept "INV-4 adding docs and non-source src/ files needs no line" "add docs"

if [[ $failures -gt 0 ]]; then
    printf '%d check(s) failed\n' "$failures" >&2
    exit 1
fi
echo "all checks passed"
