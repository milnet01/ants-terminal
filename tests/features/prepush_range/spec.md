# The pre-push hook never skips its gate on a change set it did not fully compute

`tools/hooks/pre-push` skips the local gate when every path a push changes
is docs-only. That verdict is only as good as the change set it is applied
to. `prepush_docs_only_parity` checks the docs-only decision and leaves the
change set out of scope; this contract covers the change set.

## Invariants

- **INV-1** — a rename is two paths. `git mv tools/x.py docs/x.md` changes a
  code path and a docs path, so the push runs the gate. With rename
  detection on, `git diff --name-only` lists only the new name and the push
  read as docs-only.

- **INV-2** — a range the hook could not diff runs the gate. When one ref's
  remote sha is not a local object, `git diff` fails; the hook must not read
  the failure as "no files changed" and let a second ref's docs-only range
  decide the push.

## How it is tested

`test_prepush_range.sh` builds a throwaway repository, installs a copy of
the real hook, and replaces `tools/ci_workflow.py` with a stub: the stub
forwards `docs-only` to the real runner, so the real decision is used, and
answers `run` by printing `GATE-RAN` instead of building. A push that skips
prints the hook's docs-only line; a push that gates prints `GATE-RAN`.
