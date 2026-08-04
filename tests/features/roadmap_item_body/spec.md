# roadmap_item_body — `item.body` and trailer suppression

Feature contract for **ANTS-3808**. Full spec:
[`docs/specs/ANTS-3808-item-body-and-trailer-suppression.md`](../../../docs/specs/ANTS-3808-item-body-and-trailer-suppression.md).

## What this pins

Two contracts shared one column and meant opposite things by it. The migration
copied the reader's `body` — whose first line is the id-and-headline text and
whose rest is every continuation, trailer included — verbatim into `item.body`,
while `RoadmapRender` emitted a synthesised head line, **then** `body`, **then**
the trailer again from the columns. Every migrated bullet therefore rendered its
headline twice and each trailer key twice.

The repair is two-sided, and this directory holds the tests for both halves:

- the migration strips the render's own head-line **prefix**, at a boundary the
  reader records (`BulletRecord::headlineEnd`) rather than one re-derived by
  matching the stored headline back against its own line;
- the render emits a trailer key from its column only when the body would not
  already re-parse to that same **value**, asked through
  `RoadmapParse::trailerValuesIn()`.

## Cases

| Case | Invariant | Asserts |
|---|---|---|
| `Inv1NoDuplication` | INV-1 | a rendered bullet carries its headline once and each trailer key's **canonical value** once — over a suppressed bullet, an empty-residual bullet, and the two-value case where the key *literal* legitimately appears twice |
| `Inv2SingleGrammar` | INV-2 | `RoadmapParse` is the only bullet grammar under `src/`, outside the enumerated exemptions |
| `Inv3RenderReaderAgree` | INV-3 | re-parsing a rendered bullet yields the same five trailer values the store holds, in both the suppressed and unsuppressed branch |
| `Inv4AccessorAgrees` | INV-4 | `trailerValuesIn()` reproduces every § 2.2.1 normalisation step, including the two rows with no `parseBullets()` counterpart (`offset`, `anchored`) in **both** polarities |
| `Inv5NoBodyLoss` | INV-5 | no bullet text is lost across migrate-then-render, over § 2.1's four shapes |

## The two cases that carry the weight

**`Inv5NoBodyLoss` is the reason the rule is a prefix strip and not a
first-line drop.** A native bullet takes its headline from the bold token only,
so text after the closing `**` exists nowhere but `body`'s first line, and for a
single-line bullet it is the item's entire substance. The affected share of this
project's own `ROADMAP.md` is between a seventh and a third depending on whether
a soft-wrapped bold headline counts — ANTS-3808 § 2.1 measured **241 of 1646**
on 2026-08-04 and an independent re-count the same day **539 of 1666**. The
ratio is the durable claim, not either pair: the denominator moves with every
roadmap append. Its **GFM row** is what proves `headlineEnd` is set on every headline
branch and not just the bold one: a GFM bullet writes neither `[id]` nor `**`,
so an implementation recording the offset only in the `rxBold` branch leaves it
at `-1` there, strips nothing, and the duplication returns.

**`Inv4AccessorAgrees` needs both polarities of `anchored` or it asserts
nothing.** Computed off the CAPTURE's offset rather than the MATCH's, the field
is false on every key of every bullet — every pattern puts literal text between
the line start and group 1 — so a fixture set that only ever expects `false`
passes against an always-false implementation.

## Out of scope

- The write-side `body_shadowed` refusal built on `TrailerMatch::offset` /
  `anchored` — **ANTS-3809**.
- Anchoring `rxSource` / `rxLanes` to close § 2.3.1's corner — **ANTS-3811**.

Fixtures are this directory's own. `tests/features/roadmap_migrate_archive_root/`
scopes itself to preamble round-tripping and lists bullet-body fidelity as out of
scope, so reaching into its internals would couple two deliberately split
contracts.
