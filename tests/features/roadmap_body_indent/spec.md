# A bullet body keeps its own shape — ANTS-4554 / ANTS-4558 / ANTS-4557

**Status:** implemented (2026-08-20)

## Problem

`parseBullets()` collected a bullet's continuation lines with
`cont.trimmed()` — each line stripped of its own leading whitespace. The
two-space indent the FORMAT owns and the extra two a nested sub-bullet
carries were removed by the same call, so:

- **ANTS-4554** — the re-render put two spaces back on every line, and a
  sub-bullet written at four spaces came back at two. In Markdown that
  re-parents it: the sub-bullet's continuation becomes a continuation of
  its PARENT. A rendering change, not a whitespace change, and it landed
  in the store on every migration.
- **ANTS-4558** — a body read back through `roadmap_query` came out
  flush-left. Mostly cosmetic, with one real hazard: read a body, edit
  it, write it back — the normal shape of `amend_body` work — and the
  structure the file had is gone, while a session comparing its
  remembered text against the file gets a `body_match_not_found` it
  cannot explain.

**ANTS-4557** was filed as an investigation because two projects
measured opposite things in the same week. Both were right. The store's
`item.body` COLUMN holds prose that ends before the metadata (what
AI_Prompts dumped). The verb's `body` FIELD was built by rendering the
item to markdown and re-parsing it (`RoadmapSource::appendRecord`), so
it carried the head line and the render's trailer block back out (what
Snatch measured). The column and the field were different things with
one name.

## Contract

**A body is the bullet's continuation lines, dedented by their COMMON
leading whitespace.** The common edge is the format's own two spaces;
anything deeper is the author's structure and survives. A body whose
every line sits deeper than two loses that shared depth — the
alternative, stripping a fixed two, leaves body text indented in the
store and in every read, and the corpus writes the common-edge shape.

**The head line is not part of the emitted body.** It is `headline` and
`headline_oneline` in the same envelope. `BulletRecord::body` still
carries it, because the trailer grammar reads the head and the body as
one string; `BulletRecord::bodyProse` is the block alone, and that is
what `roadmap_query` emits.

**The trailer lines stay.** They are continuation lines of the bullet on
both backends, and `Source:` / `Layman:` text is carried by no other
field the list emits — so removing them would make the `query=` keyword
filter blind to it.

**And a trailer line may be COMPOSED rather than quoted.** The render
emits one for every column whose key the stored prose does not already
declare at a line start (INV-12), so a body that never wrote `Lanes:`
comes back carrying one. The field answers *what does this bullet say in
`ROADMAP.md`* — and it does say it, because the file is generated from
the store. It does not answer *does the stored body declare this field*;
nothing distinguishes a composed line from a quoted one, and nothing can,
because the markdown backend parses the rendered file and has no way to
know which lines a renderer wrote. Ask the `item.body` column for that
question (ANTS-4599).

**What this does not repair:** a project migrated before this change
holds flattened bodies in its store, and its rendered file is flat too.
The depth is recoverable only from git history.

## Invariants

- **INV-1** — a body read through `roadmap_query include_body` keeps the
  indentation below the common edge: a four-space sub-bullet comes back
  at two, an indented command line keeps its extra depth.
- **INV-2** — the emitted body does not carry the head line: no id
  token, no bolded headline, and it begins at the first continuation
  line.
- **INV-3** — one rule, both backends: a migrated project and a
  markdown project answer the same fetch with the same body, byte for
  byte. On the store side that is the render → re-parse round trip as
  well as the read.
- **INV-4** — a re-render preserves depth: after any `roadmap_log` op on
  a migrated project the four-space sub-bullet is still at four and the
  command line still deeper.
- **INV-5** — and the trailer lines are still in the body, deliberately.
- **INV-6** — a trailer line for a column the stored body does not
  declare is composed into the body, and into the rendered file with it.
  INV-5's fixture cannot see this: its body declares every key its
  columns carry, so the render suppresses all of them.
