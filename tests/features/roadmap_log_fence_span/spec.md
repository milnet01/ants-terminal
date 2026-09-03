# roadmap_log fence-span conformance (ANTS-4404)

## Context

ANTS-4403 removed a hand-rolled fence test from the migration's
`walkSource()` after one line of prose at `ROADMAP.md:31081` opened a fence
nothing closed, masking 481 of 2,040 items. ANTS-4404 recorded that the same
naive predicate — `trimmed().startsWith("```")` toggling a bool — was still
live in four other walkers, and stated that whether they misread the same
file was **not yet measured**.

Measured 2026-08-15, and the answer is worse than truncation. The two walkers
in `remotecontrol.cpp` (`walkGfmBullets`, `walkAntsV1Bullets`) feed the
`insideFenced` flag that every `roadmap_log` write op consults. Against the
live roadmap:

```
roadmap_log op:amend_body id:ANTS-4383
  -> anchor_unsafe_context
     "located bullet is inside a fenced code block —
      refusing to edit — the fence opens at line 31099"
```

`ROADMAP.md:31099` is

```
  a ```` ```python ```` because that is what `ruff format` formats
```

— a four-backtick span quoting a three-backtick literal. CommonMark § 4.5
forbids a backtick in a backtick fence's info string precisely so this stays
a paragraph (ANTS-3655). The naive predicate reads it as an opener, nothing
closes it, and **every bullet below line 31099 becomes uneditable**: flip,
flip_batch, annotate, amend_body and amend_headline all refuse.

So this is not a truncation bug in a read path. It is a write outage over the
tail of the roadmap, and it is silent about its real cause — the refusal names
an innocent line.

## Contract

`walkGfmBullets` and `walkAntsV1Bullets` take fence extents from
`MarkdownScan::fenceMask()` (ANTS-3603), which is the same repair ANTS-4403
applied to `walkSource()`. The rule is stated once, in `markdownscan.cpp`.

## Invariants

- **INV-1** — a bullet following a four-backtick span that quotes a
  three-backtick literal is editable. `amend_body` against it succeeds and
  does not return `anchor_unsafe_context`.
  *Test:* `Inv1QuotedFenceDoesNotMaskFollowingBullet`. **Fails on assertions
  against the pre-fix walk** (returns `anchor_unsafe_context`).

- **INV-2** — a bullet genuinely inside a real, unterminated ``` fence is
  still refused with `anchor_unsafe_context`. Masking is not weakened to buy
  INV-1.
  *Test:* `Inv2RealFenceStillMasks`. Passes before and after.

- **INV-3** — a bullet following a real, *closed* ``` block is editable; the
  closer ends the fence.
  *Test:* `Inv3ClosedFenceReleasesFollowingBullet`. Passes before and after.

- **INV-4** — a fence indented under a list item (the indent-4/5/6 shape this
  project's own ROADMAP.md carries) still masks its body. This is the
  regression a naive tightening of the indent rule would cause, and ANTS-4403
  pinned the same property for the migration walk.
  *Test:* `Inv4IndentedFenceUnderBulletStillMasks`. Passes before and after.

- **INV-5** (ANTS-4823 repair 2) — a fence opened inside a bullet's *body* ends
  with that body. A later bullet is editable even though the fence is never
  closed. The write-side escape (`rcEscapeUnclosedFence`) only covers text
  written through the verb, so a hand-written or legacy body can still open one;
  before this rule that body took every bullet under it down with it, and the
  refusal named an innocent bullet.
  *Test:* `Inv5BulletBodyFenceDoesNotEscapeItsBullet`. **Fails on assertions
  against the pre-fix walk** (returns `anchor_unsafe_context`).

  The rule keys on where the fence OPENS, not on whether it is terminated —
  which is what keeps INV-2 intact: a fence opened in section prose is inside no
  bullet's body, so the bullet under it is still genuinely fenced.

## Deliberately not covered

- `testauditengine.cpp:1604` and `changelogquery.cpp:151`, the other two sites
  ANTS-4404 names. They read different documents and neither gates a write;
  they are measured separately rather than folded in, for the same reason
  ANTS-4403 did not widen into a sweep.
