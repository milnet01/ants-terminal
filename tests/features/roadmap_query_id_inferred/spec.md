# ANTS-4575 — say which roadmap ids were INFERRED from a bold prose lead-in

## Background

`roadmap_query` reports an `id` for every bullet, and a caller cannot tell
a written-down id from a guessed one. On a GFM task-list roadmap the reader
adopts a leading bold span as the item's id — `- [ ] **Terrain System** — …`
becomes id `Terrain System` — and nothing in the envelope says so. Vestige's
roadmap is 989 such bullets, so a session planning work there is reading
guessed identities without knowing it.

The parser's own comment (`roadmapparse.cpp`, the ANTS-1438 block) states the
limit that makes this worth surfacing: nothing in the text can separate a
prose lead-in from a real multi-word id, so two bullets sharing a lead-in
silently collide. That is the argument for ANTS-3771; this item is the
cheaper half — say which ids are in that class.

## What was decided, and what was rejected

Decided 2026-08-21 by the user: **declared vs inferred**, two values.

The alternative was the store's own three-word vocabulary
(`parsed` / `synthesised` / `quarantined`), and it was rejected on a
measurement already in the ANTS-4575 bullet: it labels all 989 Vestige
bullets `quarantined`, which tells a caller only that this project's ids are
not ANTS-shaped — something they can already see.

Three consequences of that choice, settled before any code was written.

**No new `BulletRecord` member.** The discriminator is derivable from two
fields both backends already fill, so ANTS-3793's 22-member census and its
INV-2 field table do not move.

**The discriminator is `id == boldId`, NOT a test on `idToken`.** `idToken`
is filled *from* `boldId` when the leading slot holds no bracket, so it
cannot separate the two cases. `id == boldId` is exactly the branch that
adopts a bold span as the id.

**The field is `id_inferred: true`, gated.** Not `id_origin`: the store
column of that name carries the rejected vocabulary, and one name for two
answers is the wart `format` already has across the verbs.

## The store backend, and why INV-2 is not at risk

ANTS-3793 INV-2 requires both backends to produce the same `BulletRecord`s,
and it compares the store against markdown parsing **the rendered file**.
The render writes every id in brackets, so both backends read a declared id
there and agree.

The flag can therefore only ever fire on a markdown-served project — which
is precisely the population it is for, since INV-1 routes a recognisable
non-`ants-v1` dialect to markdown. Stated as a consequence rather than a
limitation: once a project is migrated and rendered, the inference has been
made permanent in the text, and the flag correctly goes quiet.

## Invariants

### INV-1 — a GFM bold lead-in reads inferred

`- [ ] **FW W5** — Add a reference-regression spec.` yields a record for
which `RoadmapParse::idWasInferred()` is **true**. This is the Vestige shape
and the reason the item exists.

### INV-2 — a declared bracket id does not

`- ✅ [ANTS-0001] **Normal bullet.**` yields `idWasInferred() == false`. The
id was written down.

### INV-3 — the ants-v1 bold-dotted adoption also reads inferred

`- 📋 **Cl9.** Short headline here.` yields **true**. ANTS-1987 adopts a
head-anchored ID-shaped bold token as the id on the native branch too, and
an adoption is an inference — the predicate is not GFM-only. This is the
invariant that fails if the implementation guards on `format`.

### INV-4 — a head-anchored bare-bracket id does not

`- 📋 [Cb7] **Bracketed headline.**` yields **false**. ANTS-1987 adopts this
shape as the id, but it was written in brackets, so it is declared.

### INV-5 — an id-less bullet is never inferred

A bullet the reader assigns no id at all yields **false**. An empty id is not
a guessed one, and a caller filtering on this flag must not see it.

### INV-6 — a synthetic content-hash id is not inferred

A GFM bullet with no bold span at all gets a content-hash id and
`synthetic == true`; `idWasInferred()` is **false**. The two fields answer
different questions and must never both fire on one bullet — `synthetic`
already tells a caller that id was manufactured.

### INV-7 — the predicate follows the ID, not the bullet

A bold-lead-in bullet whose body cites a `[PROJ-NNNN]` token reports
**false**, because the id the envelope reports is that bracket token and not
the bold span. Pins current behaviour: `rxId` matches body-wide and wins over
the bold branch, which the `idToken` comment already records as a mis-read of
a citation. The mis-read is out of scope here; this invariant exists so that
changing it is a deliberate act rather than a silent side effect.

### INV-8 — every bullet-fill site that emits `bold_id` also emits `id_inferred`

`roadmap_query` fills its bullet objects at five sites. The count of
`id_inferred` emissions across the RemoteControl sources is **>= the count of
`bold_id` emissions**, so a sixth fill site added later cannot carry one
without the other. Same posture as ANTS-1646 INV-2, and the reason it is
needed here is that `bold_id` itself was added at three sites and has since
grown to five.

### INV-9 — the flag reaches the caller

`cmdRoadmapQuery` over a roadmap carrying all three id origins at once emits
`id_inferred: true` on the adopted bullet, and **omits the key entirely** on
the declared and synthetic ones — the field is gated, not emitted as `false`.

INV-8 proves the emit line exists in the source; only this proves the field
survives into the envelope. That gap is real rather than theoretical:
ANTS-1881's `headline_only` mode rebuilds each bullet as a four-key object
and so strips this field on purpose, which is a live example of a projection
losing it.

## Test plan

Behavioural against `RoadmapParse::parseBullets` over synthetic ants-v1 and
github-task-list fixtures (no real `ROADMAP.md`), plus one source-scrape for
INV-8's site coverage.

INV-1, INV-3, INV-8 and INV-9 FAIL against pre-fix code — `idWasInferred`
does not exist and nothing emits the field. Verified by stubbing the
predicate to `return false` and running: INV-1, INV-3 and INV-8 went red on
their assertions (not on a compile error) while the other five stayed green,
which is what shows those five are not passing vacuously.
