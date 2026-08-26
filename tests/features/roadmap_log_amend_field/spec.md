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
