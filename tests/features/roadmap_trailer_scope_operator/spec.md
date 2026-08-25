# A C++ scope operator is not a trailer declaration

**ID:** ANTS-4608
**Status:** shipped
**Surface:** `RoadmapParse::trailerValuesIn()` and the trailer-key patterns
in `src/roadmapparse.cpp`.

## Contract

`roadmap-format.md` § 3.5 names the label `Kind: <kind>.` — a key, a colon,
a value. `Foo::bar` is a different token, and no render emits one as a
trailer. The patterns matched inside it anyway, because they are
deliberately un-anchored (ANTS-2058) so an inline trailer is found mid-line.

In this codebase the collision is routine, not exotic: any symbol whose
scope name ends in a key word hits it — `Source`, `Kind`, `Lanes`,
`Evidence`, `Layman`.

The visible cost was `roadmap_log` refusing `body_shadowed` on a note that
merely named a symbol, and that refusal was *correct about the consequence*
— a re-parse really would have read the rest of the line as the column. The
predicate underneath it was what was too wide.

## Invariants

- **INV-1** — **A key followed by a second colon declares nothing.** A body
  naming a scoped symbol parses with that column unset. *Test:*
  `Inv1ScopeOperatorDeclaresNothing`, over all five keys.
- **INV-2** — **A real declaration on the same body still wins.** The guard
  removes a false match without removing a true one sharing the body.
  *Test:* `Inv2RealDeclarationStillParses`.
- **INV-3** — **A scope operator does not truncate a neighbouring value.**
  The stop-marker patterns carry the same lookahead, so a symbol inside a
  `Source:` value no longer ends it early. *Test:*
  `Inv3ScopeOperatorDoesNotTruncate`.
- **INV-4** — **The bold form is unaffected.** `**Kind:**` has an asterisk
  after the colon, not a colon, so the lookahead cannot reject it — the
  guard can only ever narrow the match to what the standard already names.
  *Test:* `Inv4BoldLabelStillParses`.
