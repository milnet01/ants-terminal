# The pre-push docs-only set matches ci.yml's paths-ignore (ANTS-4726)

`tools/hooks/pre-push` skips the local gate when every changed path is
"docs-only". Its path list is a hand-maintained twin of `ci.yml`'s push
`paths-ignore`, and nothing checked that the two agree.

That is the shape ANTS-4392 names for `tools/ci-parity.sh` and ANTS-4391 is
what it cost: a parallel implementation drifts, and the drift is invisible
locally because both files look reasonable on their own. The repair for the
class is a static check that the two recipes agree, never a more careful copy.

## Invariants

**ANTS-5322 retired the twin.** The hook no longer keeps a list: it pipes the
changed paths to `tools/ci_workflow.py docs-only`, which reads ci.yml's push
`paths-ignore` and applies GitHub's glob rules. So there is nothing left to
agree, and the invariants are about the one decision.

- **INV-1** — the hook holds no docs-only list of its own (`docs_only_re` is
  gone) and asks the runner with the changed paths.

- **INV-2** — the decision follows ci.yml and is anchored at the start of the
  path: every literal `paths-ignore` entry reads as docs-only, a file under
  `docs/` does, `src/docs/x.md` does not, and a push mixing a docs path with a
  code path runs the gate. Skipped when python3 or PyYAML is absent, since the
  hook itself refuses then.

## Out of scope

Whether the hook's computed change set is the true push range. ANTS-4726
observed a misclassification that this parity check cannot explain and could
not reproduce; the hook now logs the set it decided on, which is what a live
recurrence needs. This invariant is about the two lists agreeing, not about
the input either one is applied to.
