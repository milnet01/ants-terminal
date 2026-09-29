# Writing a trailer column after creation — ANTS-4667

**Status:** implemented (2026-08-26)

## Problem

`roadmap_log` could CREATE a `layman` / `kind` / `source` / `lanes` /
`evidence` at append time and never change one afterwards.

`amend_body` edits the stored BODY column. The trailer lines are
COMPOSED at render time from their own columns (ANTS-4599), so they are
not in the body and `amend_body` cannot reach them. What turned a
missing feature into a TRAP is that `roadmap_query include_body:true`
returns those composed lines INSIDE `body` — so a caller reads the text
back verbatim, passes it as `old_text`, and is told
`body_match_not_found` about a string it has just read. The refusal
mentioned neither composed trailers nor any way out.

The documented workaround is ONE-WAY. Declaring `Layman:` at a line
start inside the body does set the column, last-wins, and cannot be
withdrawn: the write path recomputes the column by re-parsing the
amended body, so deleting the declaration yields no Layman and the write
refuses `render_gate_unmet`. The render then emits a plain `Layman:`
line where the composed form is bold, so a project that corrects one
Layman carries two styles it can never reconcile.

`roadmap-format.md` makes Layman REQUIRED, and it is what the Roadmap
dialog shows on the card face. So the one field written for the
non-technical reader was the one that could not be corrected.

## Contract

**`op:"amend_field"` replaces one column outright** — `id` + `field` +
`value`, `dry_run` previewable.

**Not a mode of `amend_body`.** It takes no `old_text`: it replaces a
value rather than patching a matched substring, so the body machinery
would be dead weight around it.

**Store-only, and id-only.** On a markdown project the trailer line IS
body text and `amend_body` already reaches it, so this refuses
`unsupported_format` naming that route. An id is the store's own key; a
headline or anchor locator would re-introduce an ambiguity the key
removes, on a write that replaces rather than matches.

**A body declaration shadows the column, so writing under one is
refused.** The declaration wins at render AND is re-parsed into the
column by the next body write — the column write would be invisible now
and reverted later, two ways of being wrong. `field_shadowed_by_body`
names `amend_body` as the route that works.

**Only `layman` is nullable.** `kind` and `source` are `TEXT NOT NULL`
with no default (ANTS-4576), so an empty value is refused here by name
rather than reaching the caller as a raw SQLite constraint string.

## Invariants

- **INV-1** — `amend_field` sets the column and the render publishes it.
  *Test:* `Inv1SetsColumnAndRenders`.
- **INV-2** — a body that declares the key at a line start refuses
  `field_shadowed_by_body` and writes nothing. *Test:*
  `Inv2ShadowedByBodyRefuses`.
- **INV-3** — a field outside the five trailer columns refuses
  `bad_args`. *Test:* `Inv3UnknownFieldRefused`.
- **INV-4** — an empty value on a NOT NULL column refuses `bad_args`;
  `layman` accepts one. *Test:* `Inv4NotNullEmptyRefused`.
- **INV-5** — `dry_run:true` writes neither the column nor the file.
  *Test:* `Inv5DryRunWritesNothing`.
- **INV-6** — `lanes` accepts an array of strings and stores it
  canonically. *Test:* `Inv6LanesAcceptsArray`.
- **INV-7** — `amend_body` given an `old_text` naming a trailer key
  returns a hint naming `amend_field`, rather than a bare
  `body_match_not_found`. This is the trap's own redirect and is the
  half that fires for a caller who has not read this contract. *Test:*
  `Inv7AmendBodyRedirectsToAmendField`.
- **INV-8** — an id the store does not hold refuses `bullet_not_found`.
  *Test:* `Inv8UnknownIdRefused`.

- **INV-9 (ANTS-5094)** — `layman` and `source` take append's control-character
  strip and caps, and each `evidence` element folds a newline or comma to a
  space, so a value cannot publish an extra ROADMAP.md line or split an
  element. `roadmap_query` `mode:"report"` refuses an unparseable `since` or
  `until` with `bad_args` rather than changing the window.

- **INV-10 (ANTS-4948)** — `field:"section"` moves an item to the section
  whose slug is `value`, filed after that section's last element, and the
  render publishes it. `locators[]` of `{id}` moves several in one call, in
  locator order. Every id resolves before anything is written, so one
  unknown id refuses `bullet_not_found` and moves nothing. An unknown slug
  refuses `section_not_found` with `candidates`. An item already in the
  destination is reported in `already_there` and nothing is written.
  *Tests:* the `Ants4948*` cases.

## Out of scope

- **Rows already damaged by the one-way workaround.** A body that
  declares the key still shadows the column, and deleting the
  declaration still trips the render gate. Making those reconcilable is
  the item's option (a) — have the render compose a body-declared
  trailer in the same bold form as a column one. Measured while
  implementing this: `RoadmapParse::rxLayman()` already accepts both the
  plain and the bold spelling, so that change would round-trip through
  the parser. Filed separately rather than folded in, because it changes
  the rendered output of every project carrying a body-declared trailer.
- `headline` (`op:"amend_headline"`, ANTS-4668) and `status`
  (`op:"flip"`), which have their own ops.

## ANTS-4841 — set_body is not held to the fragment cap

op:"set_body" accepts a `new_text` up to 65536 characters, because it replaces
a whole body. amend_body and amend_headline keep the 4096-character cap on
their fragments. A set_body `new_text` past its own cap refuses `too_large`.
*Test:* `RoadmapLogSetBody.Ants4841WholeBodyCapIsNotTheFragmentCap`.

## ANTS-5385 — `op:"amend_field_batch"`

`amendments:[{id, field, value}]` applies several trailer-column changes in
one commit and one render. Each entry runs `amend_field`'s own checks. So the
render gate judges the state after every change, and a Kind plus a Layman for
one open item land together where a Kind alone is refused.

- A refused entry lands in `skipped[]` with its `index`, `code` and `error`,
  and costs only itself. A second entry for a column an earlier entry already
  sets is refused `bad_args`; the first value stands.
- When every entry is refused the call refuses and writes nothing: the shared
  code when all failed the same way, else `bad_args`. An absent or empty
  `amendments` refuses `missing_field`. `field:"section"` is refused per
  entry, since moving items is `amend_field`'s `locators` form.
- ANTS-5566: when every entry is refused and the rows are the same apart
  from `index` and `id`, `skipped[]` holds the first row only and
  `skipped_uniform` is true; `skipped_count` is still the full count. Rows
  that differ in anything else keep every row.
  The same rule covers every batch op's all-refused reply.
- The reply carries `amended[]` ({id, field, previous, value}),
  `amended_count`, `skipped[]` and `skipped_count`.

*Tests:* the `RoadmapLogAmendFieldBatch.*` cases.

## ANTS-4669 — `op:"amend_batch"`

`locators[]` of `{id, old_text, new_text}` applies `amend_body`'s edit to each
item's stored body in one read, one commit and one render. Each locator runs
`amend_body`'s checks: the trailer guard (`body_shadowed`), the unique
wrap-tolerant match (`body_match_not_found`, `body_match_ambiguous`,
`body_match_wrapped_block`), `bullet_not_found`. A refused locator lands in
`skipped[]` with its index; two aimed at one item apply in order, the second
matching the first's result. All refused writes nothing. Store-only: a
markdown project refuses `unsupported_format` and is pointed at
`amend_body`. *Tests:* `RoadmapLogAmendBatch.EditsSeveralBodiesInOneCall`,
`RoadmapLogAmendBatch.AllRefusedWritesNothing`.
