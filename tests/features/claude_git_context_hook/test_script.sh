#!/bin/bash
# Claude Code UserPromptSubmit git-context hook — behavioral test.
# See tests/features/claude_git_context_hook/spec.md.
#
# Extracts the canonical script from src/settingsdialog.cpp (single
# source of truth — no drift), writes it to a temp file, and runs it
# through five scenarios:
#
#   1. Not a repo           → exit 0, empty stdout.
#   2. git missing from PATH → exit 0, empty stdout.
#   3. Clean repo           → <git-context> block, zero counts, no
#                              (ahead …, behind …) clause (no upstream).
#   4. Dirty repo (1/1/1)   → exact counts in each line.
#   5. CLAUDE_PROJECT_DIR   → honored over $PWD for the git queries.

set -eu

: "${SRC_SETTINGSDIALOG_CPP:?SRC_SETTINGSDIALOG_CPP env var must be set by CMake}"
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not available (needed to extract script from C++ source)"
    exit 0
fi
if ! command -v git >/dev/null 2>&1; then
    echo "SKIP: git not available on test host"
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

extracted="$tmp/claude-git-context.sh"
python3 - <<EOF > "$extracted"
import ast, re, sys
text = open("$SRC_SETTINGSDIALOG_CPP").read()
# Locate the QStringLiteral script body inside installClaudeGitContextHook.
# Capture everything between the opening '(' and the 'QSaveFile sf(scriptPath)'
# that immediately follows — handles the last line carrying \`");\` on its tail.
m = re.search(
    r'void SettingsDialog::installClaudeGitContextHook\s*\(.*?'
    r'const QString script = QStringLiteral\(\s*\n'
    r'(.*?)\n\s*QSaveFile sf\b',
    text, re.S)
if not m:
    sys.exit("ERROR: could not extract script block from SRC_SETTINGSDIALOG_CPP")
body = m.group(1)
chunks = []
for line in body.splitlines():
    s = line.strip()
    if not s.startswith('"'):
        continue
    # The last line may have trailing ');' etc. after the closing quote —
    # slice up to the final '"' so ast.literal_eval sees a clean literal.
    last = s.rfind('"')
    if last <= 0:
        continue
    lit = s[: last + 1]
    # ast.literal_eval handles C-style \n, \", \\ escape sequences that
    # the C++ compiler would otherwise decode.
    chunks.append(ast.literal_eval(lit))
sys.stdout.write("".join(chunks))
EOF

chmod +x "$extracted"
fail=0
report_fail() { echo "FAIL: $*"; fail=$((fail + 1)); }

# --- Test 1: not a repo ----------------------------------------------------
mkdir -p "$tmp/no-repo"
out1=$(cd "$tmp/no-repo" && "$extracted" </dev/null 2>&1 || true)
if [[ -n "$out1" ]]; then
    report_fail "not-a-repo: expected empty stdout, got:"
    printf '  %s\n' "$out1"
fi

# --- Test 2: git missing ---------------------------------------------------
# env -i purges the env; set only PATH (to a dir without git) and HOME.
out2=$(cd "$tmp/no-repo" && env -i PATH=/nonexistent HOME="$HOME" \
        "$extracted" </dev/null 2>&1 || true)
if [[ -n "$out2" ]]; then
    report_fail "git-missing: expected empty stdout, got:"
    printf '  %s\n' "$out2"
fi

# --- Test 3: clean repo on branch 'main', no upstream ---------------------
mkdir -p "$tmp/clean"
(
    cd "$tmp/clean"
    git init -q
    git checkout -q -b main 2>/dev/null || true
    git config user.email "test@example.com"
    git config user.name "Test User"
    echo hello > README
    git add README
    git commit -q -m "initial"
)
out3=$(cd "$tmp/clean" && "$extracted" </dev/null 2>/dev/null)
for needle in '<git-context>' 'Branch: main' 'Upstream: (none)' \
              'Staged: 0 file(s)' 'Unstaged: 0 file(s)' 'Untracked: 0 file(s)' \
              '</git-context>'; do
    if ! grep -qF "$needle" <<< "$out3"; then
        report_fail "clean: missing '$needle' in output:"
        printf '  %s\n' "$out3"
    fi
done
if grep -q 'ahead' <<< "$out3"; then
    report_fail "clean: (ahead …, behind …) MUST be omitted when no upstream"
    printf '  %s\n' "$out3"
fi

# --- Test 4: dirty repo — 1 staged, 1 unstaged, 1 untracked ---------------
mkdir -p "$tmp/dirty"
(
    cd "$tmp/dirty"
    git init -q
    git checkout -q -b main 2>/dev/null || true
    git config user.email "test@example.com"
    git config user.name "Test User"
    echo a > f1
    echo b > f2
    git add f1 f2
    git commit -q -m "initial"
    # Unstaged modification to f1
    echo mod >> f1
    # Staged modification to f2
    echo more >> f2
    git add f2
    # Untracked new file
    echo new > f3
)
out4=$(cd "$tmp/dirty" && "$extracted" </dev/null 2>/dev/null)
for needle in 'Staged: 1 file(s)' 'Unstaged: 1 file(s)' 'Untracked: 1 file(s)'; do
    if ! grep -qF "$needle" <<< "$out4"; then
        report_fail "dirty: missing '$needle' in output:"
        printf '  %s\n' "$out4"
    fi
done

# --- Test 5: CLAUDE_PROJECT_DIR honored over $PWD -------------------------
# Run from a non-repo cwd but point CLAUDE_PROJECT_DIR at the dirty repo.
out5=$(cd "$tmp/no-repo" && CLAUDE_PROJECT_DIR="$tmp/dirty" \
        "$extracted" </dev/null 2>/dev/null)
if ! grep -qF 'Staged: 1 file(s)' <<< "$out5"; then
    report_fail "CLAUDE_PROJECT_DIR override not honored; output was:"
    printf '  %s\n' "$out5"
fi

if (( fail > 0 )); then
    echo
    echo "$fail behavioral invariant(s) failed — see spec.md"
    exit 1
fi

echo "OK: claude-git-context.sh — 5 behavioral invariants"
