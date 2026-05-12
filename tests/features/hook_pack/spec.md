# hook_pack — ANTS-1252 conformance

Source spec: [docs/specs/ANTS-1252.md](../../../docs/specs/ANTS-1252.md).

Shell-driven harness (no C++ link, no QProcess wrapper) following the
`tests/audit_self_test.sh` pattern. Exits 0 when every assertion
passes, non-zero (with FAIL lines on stderr) otherwise.

## What this test asserts

1. **INV-1 (no-op exit 0).** Every script in `hooks/ants-*.sh`
   exits 0 when fed `printf '{}\n'` on stdin from inside the project
   root.
2. **INV-3 (preamble cap).** `ants-session-preamble.sh` stdout
   ≤ 500 bytes against `{}` stdin.
3. **INV-4 (bash-veto reason cap).** Bash veto's `reason` field
   ≤ 200 bytes for each pattern that triggers a block.
4. **INV-7 (jq-only payload parse).** `ants-bash-veto.sh` and
   `ants-read-roadmap-veto.sh` source contain `jq -r` and do NOT
   contain raw-regex against the JSON input (no `grep -P` / `sed`
   over the unparsed payload).
5. **INV-10 (silent side-effect hooks).** `ants-drift-check.sh` and
   `ants-precompact-snapshot.sh` emit zero stdout bytes against
   `{}` stdin.
6. **INV-12 (no leakable bypass token).** For each pattern that
   fires a block, the emitted block-reason JSON's `.reason` MUST
   NOT contain the literal `# ants-bypass` token. The token DOES
   appear in `ants-bash-veto.sh` source (inside the `case` glob
   that *enables* the bypass) — INV-12's concern is leakage into
   the model's input, i.e. the reason string. The override
   mechanism is documented in `hooks/README.md` only.
7. **Bash-veto behavioral hits.** Three positive cases that MUST
   produce a `decision:"block"` JSON envelope: `grep -r foo src/`,
   `git status`, `cat ROADMAP.md | grep`. One negative: `ls -la`
   produces no `decision:block` envelope (asserted as exactly
   empty stdout in the current implementation).
8. **Bash-veto bypass.** A trailing `# ants-bypass` on the proposed
   command suppresses the veto (empty stdout).
9. **Read-roadmap veto.** Block fires for full-file reads of the
   project's `ROADMAP.md`; suppressed when `offset` or `limit` is
   present, and for unrelated file paths.
10. **INV-9 silence outside project.** Each hook, run from a
    directory with NO `.ants-project` ancestor, exits 0 with empty
    stdout.
11. **INV-2 sessionId regex.** `_common.sh::ants_validate_session_id`
    accepts `abc123_-`, rejects `../foo`, `a/b`, the empty string,
    and a 65-char overflow.
12. **install-hooks.sh round-trip (also covers INV-5).** `--dry-run`
    against a tmp target produces JSON the post-install validator
    parses; live install + uninstall round-trips back to removing
    the sentinel key; a re-run on top of an existing install is
    idempotent (INV-1).
13. **INV-5 (symlink abort).** `tools/install-hooks.sh` against a
    target that `lstat`s as a symlink exits non-zero with a
    descriptive message — verified inside the install-hooks
    round-trip block.

### Not covered (deliberately)

- **INV-6** (sentinel-key fence) — partially exercised by the
  install + uninstall round-trip in assertion 12 (the sentinel key
  is set on install, removed on uninstall). A dedicated assert that
  rejects text-fence comments is omitted as the install path never
  emits comments.
- **INV-8** (jq-validate-before-rename) — would need a fixture that
  forces tmpfile corruption before rename; defer to manual smoke
  test until a regression motivates it.
- **INV-11** (drift-check `flock` PID-file) — would need a
  concurrent-fork race; defer to manual smoke test for the same
  reason.

## Why shell, not C++

The hooks are bash scripts; the install path is bash + jq; no
production code links them. A C++ harness would only proxy back to
`bash <hook>` anyway. Shell is the natural surface — same call as
the real Claude Code hook runtime.

## Project-root caveat

INV-9 cases run inside a `(cd "$outside" && …)` subshell so the
parent harness shell stays in `$REPO_ROOT` throughout — there is no
explicit cd-back step, the subshell pattern is what protects later
assertions. The outside fixture is `mktemp -d -t
ants-hook-pack-out.XXXXXX`; the install-hooks round-trip uses a
separate `ants-hook-install.XXXXXX` template. Either prefix means
a stale fixture from a crashed run is isolated from the next.

## Skip behaviour

If `jq` is not on PATH, the harness skips: INV-4 + bash-veto
behavioural cases, INV-12, read-roadmap-veto behavioural cases, and
the install-hooks round-trip. INV-1, INV-3, INV-7 (source-grep),
INV-9, INV-10, and INV-2 still run. Every skip is announced with a
`[skip]` line — never silent. CI runners without jq fail the
assertion only if jq's absence would mask a real bug (none
currently).
