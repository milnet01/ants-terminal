# A push is gated on a change set the hook fully computed

Since ANTS-5542, `tools/hooks/pre-push` is a shim. It hands the push to the
machine-wide hook (`~/.claude/githooks/pre-push`), which works out the files
the push changes, asks `tools/ci_workflow.py docs-only` whether they are all
documentation, and runs `tools/local-ci.sh` in full or with `--docs`. A
docs-only verdict is only as good as the change set it is applied to. This
contract covers that change set and the hand-off; `prepush_docs_only_parity`
covers the docs-only decision itself.

## Invariants

- **INV-1** — a rename is two paths. `git mv tools/x.py docs/x.md` changes a
  code path and a docs path, so the push runs the full gate. With rename
  detection on, `git diff --name-only` lists only the new name and the push
  read as docs-only.

- **INV-2** — a range the hook could not diff runs the full gate, with
  `ANTS_PUSH_CHANGED` unset. When one ref's remote sha is not a local object,
  `git diff` fails; the failure must not read as "no files changed" and let a
  second ref's docs-only range decide the push.

- **INV-3** — a documentation-only push runs the gate with `--docs`, and
  `ANTS_PUSH_CHANGED` names its paths.

- **INV-4** — the committed `.ants/gate.conf` governs a clone with no
  `ants.gate.*` keys of its own: a docs-only push from a clean tree at HEAD
  runs `--docs` in place. Without it the hook would gate in a cold checkout
  and never see a push as docs-only.

## How it is tested

`test_prepush_range.sh` builds a throwaway repository with copies of the
real shim and `.ants/gate.conf`, and runs the real machine-wide hook. A stub
`tools/ci_workflow.py` forwards to the real runner, so the real docs-only
decision is used. A stand-in `tools/local-ci.sh` prints how it was called
instead of building. The test exits 77 (skipped) where the machine-wide hook
or PyYAML is absent, as on GitHub's runners.
