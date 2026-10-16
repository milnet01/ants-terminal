# A commit adding a source file says how it reaches a running terminal

`CLAUDE.md` § "Hot reload is the design default" asks every new feature to
state how it reaches a running terminal without a relaunch. Since
ANTS-5721, `tools/hooks/commit-msg` checks the commit-side half: a commit
that adds a source file under `src/` carries a `Reload:` line. The hook
runs in every clone with `core.hooksPath=tools/hooks`
(`tools/setup-git-hooks.sh`).

## Invariants

- **INV-1** — a commit that adds a `.cpp`, `.h`, `.c`, `.cc` or `.hpp` file
  anywhere under `src/`, with no `Reload:` line, is refused, and the
  refusal names the added file.

- **INV-2** — the same commit with a line `Reload: <text>` is accepted. A
  bare `Reload:` with nothing after it is refused.

- **INV-3** — a commit that only modifies or deletes existing `src/` files
  needs no `Reload:` line.

- **INV-4** — a commit that adds files outside `src/`, or a non-source
  file under `src/`, needs no `Reload:` line.

## How it is tested

`test_commit_msg_reload.sh` builds a throwaway repository, copies the real
`tools/hooks/commit-msg` into it, sets `core.hooksPath`, and makes real
commits. A refused commit leaves `HEAD` where it was; the test checks
`HEAD` as well as the exit status, so a hook that is absent, or that
exits 0 by mistake, fails INV-1.

Not covered: `git commit --amend` compares the index against the commit
being amended, so a file that commit already added does not count as
added. `--no-verify` skips the hook, and needs the user.
