# roadmap_export_roundtrip — the roadmap export

Feature contract for **ANTS-3761**.
Parent spec: [`docs/specs/ANTS-3761-roadmap-export-format.md`](../../../docs/specs/ANTS-3761-roadmap-export-format.md)

§ 6 of the parent assigns INV-1, 2, 5, 12, 13, 18 and 19 to this one directory.
**All seven are in place.**

## The fixture

One synthetic three-project store, shared by INV-1, 2, 5, 13 and 18 — never the
machine's real corpus, which is not present in CI, and never anything
clock-derived, because a fixture that changes at midnight cannot support a
byte-identity invariant.

- **alpha** exercises every record type and every variant of the multi-variant
  ones: both `section` shapes, all three `element` kinds, all three `rel` target
  forms, both `citation` anchorings. Its items are inserted in an order that is
  deliberately *not* id order, one item is deleted (so rowids carry a gap no
  rebuild recreates), and `a-child` is filed under `z-parent` so the section
  order's parents-first term is observable rather than incidental.
- **beta** is the far side of the cross-project `blocked-by`.
- **gamma** is the sparse case — no legend, no id prefix, every optional column
  NULL — which is what walks § 2.4's *omitted* column rather than its *always
  emitted* one.

`ANTS-9` carries `extras.ratio = 0.000001`, the exact value ECMAScript keeps in
fixed notation and Qt writes as `1e-06`. It is in the fixture so the number
contract of INV-19 is re-proved end to end, through a stored column and a
rebuild, rather than only at the canonicaliser's own door.

## What this locks

**INV-1** round trip is byte-identical · **INV-2** every row and non-surrogate
column survives · **INV-5** the numeric-segment id sort, and it is total ·
**INV-12** peak RSS delta under 4 MiB · **INV-13** every reference resolves and
no surrogate is emitted under any name · **INV-18** the export matches the
committed golden files · **INV-19** RFC 8785 conformance.

The golden files under `golden/` are checked in and reviewed. Regenerating them
is possible (`ANTS_REGENERATE_EXPORT_GOLDEN=1`) and **still fails the test** by
design: the only route to a green run is to read the resulting diff and commit
it. The escape hatch exists because the alternative — hand-editing a golden
file — is strictly worse.

### INV-19 in detail

Four legs, and the split is not tidiness — leg 1 alone passes against exactly
the writer the parent spec rules out:

1. **The six published vector files** — `arrays`, `french`, `structures`,
   `unicode`, `values`, `weird` — byte for byte. Committed under `vectors/`
   with their provenance; see that directory's README.
2. **RFC 8785 Appendix B, Table 1** — all 24 number samples, addressed by their
   IEEE 754 bit patterns so the test says what the RFC says.
3. **The RFC's mandatory error case.** § 3.2.2.2 requires a conforming
   implementation to *terminate* on a lone surrogate; the parent's § 2.4 turns
   that into "abort and report the row", never a replacement character. A
   well-formed surrogate **pair** must still serialise — the check must reject
   invalid UTF-16, not all non-BMP text.
4. **Key order is JCS's**, not insertion or reading order. § 2.2 warns that the
   record shapes in § 2.3 are written readably for humans and are not
   byte-exact; this is what makes that warning testable.

## Must fail first

Each mutation applied to one source file, built, run, reverted. Against
`src/jsoncanonical.cpp`:

- `numberToString()` delegating to `QJsonDocument::toJson(Compact)` — the exact
  mistake § 2.2 names → **leg 2 RED**.
- The lone-surrogate check removed → **leg 3 RED**: the string serialises with
  U+FFFD substituted, which is the outcome § 2.4 forbids.
- The key comparator reversed → **legs 4 and 1 RED**.

**Leg 1 stays GREEN under the first mutation, and that is the finding.** Qt
matches JCS on all six published files; it is only Appendix B's number table
that catches it, at 21 of 24. A test built from the vector files alone would
have certified the writer this spec exists to rule out — which is the same
shape as INV-18's reason for existing, one level down: self-consistency, and
even partial external agreement, are not conformance.

Against `src/roadmapexport.cpp`:

| Mutation | RED | Green, and that is the point |
|---|---|---|
| item record carries a rowid under a reference key (`"section": <pk>`) | INV-13, INV-1, INV-18 | — |
| an omitted-when-absent column dropped (`layman`) | INV-2, INV-18 | **INV-1** — the drop round-trips byte-identically, which is exactly why INV-2 exists |
| the item comparator reversed | INV-5, INV-18 | **INV-1** — a deterministic writer built from stable data passes the round trip |
| the id sort splits **on** separators instead of removing them | INV-5, INV-18 | — |
| rule 5's raw-id tie-break removed | INV-5, INV-18 | — |
| a failed lock acquire treated as permission to proceed | INV-9 | — |
| the writer buffers the whole export and writes it at the end | INV-12 | — |
| one object emitted via `QJsonDocument::toJson(Compact)` | INV-18 | **INV-1** — see below |

**Two mutations did not redden what they were supposed to, and both were worth
the run.**

The last row is the parent spec's *own* prescribed INV-1 mutation, and INV-1
stays green under it: Qt's compact output is deterministic and re-parses to the
same doubles, so the writer, the rebuild and the re-export all agree. The
instruction asked INV-1 to catch a deterministic writer, which is the one thing
INV-18 exists because INV-1 cannot do. § 6 of the parent is amended.

Dropping `provenance` — the column INV-2's clause named — reddens INV-1 as
well, because § 2.4 emits `provenance` always and the reader refuses an export
missing it. That is the reader working correctly, but it means `provenance`
cannot demonstrate INV-2's independence; `layman` can, and does. The first cut
of the reader made this worse still: it silently wrote `''` into a `NOT NULL`
JSON column, producing a store whose *next* export would abort on a row nothing
had reported. The parent's § 2.7 now states the reader's abort obligation.
