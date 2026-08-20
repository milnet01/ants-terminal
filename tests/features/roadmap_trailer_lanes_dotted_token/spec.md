# ANTS-4597 — the lanes capture does not stop at a dotted token

Status: implemented

## Problem

`rxLanes()` captures its value with a NON-GREEDY `(.+?)` closed by
`[\.\n]` (`src/roadmapparse.cpp:334`) — the shape ANTS-4596 has just
removed from `rxLayman()`. The value therefore ends at the FIRST full
stop inside it, and for this key that stop is usually a dot inside a
filename rather than the end of the sentence.

The damage is worse than the layman key's, because a truncated LIST
loses whole members rather than trailing characters. `splitTrailerList()`
sees a shorter string and yields fewer lanes; nothing downstream can tell
that from a bullet that genuinely declared one lane.

Measured 2026-08-20 by running BOTH parsers — this file at `HEAD~` and
at `HEAD` — over the machine-global store's 1126 bodies that carry a
`Lanes:` declaration. Not against the stored `lanes` column: that was
written at migration and also carries ANTS-4542's repair, which would be
credited here. **15 bullets parse differently, 5 gain whole members, 302
characters are recovered, and nothing loses a member.**

    Lanes: remotecontrol.cpp, AuditDialog, RoadmapDialog, MainWindow, …
      before: ["remotecontrol"]                                (ANTS-1117)
      after : 6 lanes
    Lanes: RoadmapDialog, docs/standards, .claude/bump.json, packaging/…
      before: ["RoadmapDialog", "docs/standards"]              (ANTS-1125)
      after : 5 lanes
    Lanes: commits.md.
      before: ["commits"]                                      (CFG-0133)
      after : ["commits.md"]

Two distinct losses. The first two lose MEMBERS, and the render then
emits the short list terminated by a full stop, which reads as a correct
declaration. The third loses only an extension, turning `commits.md`
into `commits` — still a real-looking lane, and therefore harder to
spot. Across the same store ZERO stored lane names contain a dot, which
is the asymmetry that makes the class visible at all.

This is ANTS-3382's reasoning for a third time: its comment says an
end-of-line capture is needed because a `[^\.\n]` stop "would truncate at
the extension". Applied to `Evidence:`, then to `Source:` (ANTS-3764),
then to `Layman:` (ANTS-4596), and never here.

## Surface

- `src/roadmapparse.cpp` — `rxLanes()`, and the `lanes` extraction in
  `trailerValuesIn()`.

## Rule

The lanes value runs to the end of its line, continues across a wrap on
`matchLastIn()`'s existing terms, stops at the first following trailer
declaration, and then ends at its SENTENCE stop.

**The sentence stop, not the end of the line**, and this is the key's one
departure from `Layman:` and `Source:`. Those hold free prose, where
absorbing a following sentence costs some over-long text; a lane run is
one clause, and the surplus is SPLIT into bogus lanes that `subsystem`
and `indie_review_partition` then key on. The existing `sentenceStop()`
is the rule the old `[\.\n]` was reaching for and got wrong: a full stop
at end-of-value or before whitespace ends the value, a dot inside a token
does not. It is already the continuation walk's terminator, so both
halves of this key stop on one rule, and it subsumes the trailing-period
chop the sibling keys do by hand.

**The stop and the chop land BEFORE the split**, not after it:
`splitTrailerList()` is applied to the normalised text, so no member can
carry the sentence period and no member can be a fragment of a following
declaration.

The stop set for this key names `Kind`, `Layman`, `Source` and
`Evidence` and NOT `Lanes` itself. `rxTrailerKey()` is built to serve
`Source:` and does name `Lanes`, so reusing it here would end a lane list
at itself; ANTS-4596 hit the same trap for the layman key and added its
own set for exactly this reason.

## Invariants

- **INV-1** — Every member after a dotted one survives: a five-lane
  declaration whose first lane is `remotecontrol.cpp` stores five lanes.
- **INV-2** — A lane's own file extension survives: `CMakeLists.txt`
  stores whole, not `CMakeLists`.
- **INV-3** — The single trailing full stop is still removed, and no
  stored lane ends in punctuation.
- **INV-4** — A following declaration on the same line still ends the
  list, and is itself still read. Before the fix the first-period stop
  produced this by accident; an end-of-line capture has to do it
  deliberately, so this is the regression the fix is most likely to
  cause. Asserted for `Source:` — the key `rxTrailerKey()` omits — and
  for `Kind:`.
- **INV-5** — ANTS-4542 still holds for this key: a lane list hard-wrapped
  across a line break is rejoined rather than truncated at the wrap.
- **INV-6** — A LEADING dot survives: `.claude/bump.json` is one lane, and
  the members after it are kept.
- **INV-7** — Ordinary prose written after the declaration on the same
  line is not split into lanes. Caught against the live store, not
  reasoned about: a first draft stopping at the end of the line repaired
  the dotted runs and turned `Lanes: hooks. Verify the resulting
  predicate table still matches docs/specs/ANTS-2141.md …` into one lane
  carrying the whole sentence.

## Tests

`test_roadmap_trailer_lanes_dotted_token.cpp` drives the pure static
`RoadmapDialog::parseBullets` and asserts the parsed `lanes`, `kind` and
`source` fields.
