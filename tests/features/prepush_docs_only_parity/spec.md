# The pre-push docs-only set matches ci.yml's paths-ignore (ANTS-4726)

`tools/hooks/pre-push` skips the local gate when every changed path is
"docs-only". Its path list is a hand-maintained twin of `ci.yml`'s push
`paths-ignore`, and nothing checked that the two agree.

That is the shape ANTS-4392 names for `tools/ci-parity.sh` and ANTS-4391 is
what it cost: a parallel implementation drifts, and the drift is invisible
locally because both files look reasonable on their own. The repair for the
class is a static check that the two recipes agree, never a more careful copy.

## Invariants

- **INV-1** — Every entry in `ci.yml`'s push `paths-ignore` is matched by the
  hook's `docs_only_re`, and every alternative in that regex appears in
  `paths-ignore`. Agreement is checked in BOTH directions: an entry only CI
  ignores makes the hook run a gate CI will not, which is merely wasteful; an
  alternative only the hook holds makes it skip a gate CI will run, which is
  the failure ANTS-4726 observed.

- **INV-2** — The regex is anchored at the start of the path. Unanchored, a
  path containing `docs/` anywhere would read as documentation.

## Out of scope

Whether the hook's computed change set is the true push range. ANTS-4726
observed a misclassification that this parity check cannot explain and could
not reproduce; the hook now logs the set it decided on, which is what a live
recurrence needs. This invariant is about the two lists agreeing, not about
the input either one is applied to.
