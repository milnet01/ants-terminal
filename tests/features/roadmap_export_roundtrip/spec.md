# roadmap_export_roundtrip — the roadmap export

Feature contract for **ANTS-3761**, and — since 2026-08-03 — for
**ANTS-3796 / ANTS-3797**.
Parent specs:
[`docs/specs/ANTS-3761-roadmap-export-format.md`](../../../docs/specs/ANTS-3761-roadmap-export-format.md) ·
[`docs/specs/ANTS-3796-section-record-completeness.md`](../../../docs/specs/ANTS-3796-section-record-completeness.md)

§ 6 of ANTS-3761 assigns INV-1, 2, 5, 12, 13, 18 and 19 to this one directory.
**All seven are in place.** § 6 of ANTS-3796 adds its INV-1, 2, 3, 5 and 7 —
also all in place, and named `Ants3796Inv<N>` in the source because both specs
number from 1 and a bare `Inv2` would resolve two ways.

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

### ANTS-3796 / ANTS-3797 — what the section record carries

**INV-1** document order survives the round trip · **INV-2** `section.source_path`
survives it · **INV-3** the column diff below fails *by default* · **INV-5** the
`(position, slug)` sort key is total · **INV-7** the rebuild importer refuses a
section record missing either new field, with no partial store.

**The ordering fixtures are built in-test and are deliberately not
golden-backed.** INV-1's three sections (`zeta` level 2, `alpha` level 3 under
it, `mid` level 2) and INV-5's same-position pair live in their own temp stores.
They exercise an *ordering*, which a committed golden cannot witness any better
than an assertion can — and seeding five sections into `alpha` would have
rewritten every record in `alpha.jsonl` in the same pass that changed the record
shape, making INV-18's reviewed diff unreadable.

The three-section shape is the whole of INV-1's power: document order
(`zeta, alpha, mid`), slug order (`alpha, mid, zeta`) and the export's
`(depth, slug)` emission order (`mid, zeta, alpha`) are three *different*
sequences. A two-section fixture, or a three-section one that happened to be in
slug order, passes against a rebuild that recomputes the ordinal. The test also
asserts that emission order ≠ document order before trusting its own result,
so it cannot pass for that reason by accident.

**INV-2's column diff is derived from the schema, not written out.** Since
ANTS-3796 § 2.5 the per-table projection reads `PRAGMA table_info` on *both*
stores and `PRAGMA foreign_key_list` for the substitutions, over three sets:
derived (every column), excluded (`project.root` plus each table's own surrogate
PK, named), and substituted (every foreign-key rowid, mapped to the stable
rendering compared in its place — `par.slug`, `i.id_fold`, `p.export_slug`). A
foreign-key rowid with no map entry **fails** rather than being skipped, and the
map is a parameter so INV-3 can inject an incomplete one. This inverts the
default: a column added to the schema used to be absent from the diff and pass,
and now it is present and fails until the export learns about it. That inversion
is the whole reason ANTS-3797 existed — `section.source_path` was dropped by
both export legs and survived a shipped INV-2 for two specs.

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

### ANTS-3796 / ANTS-3797 mutations (2026-08-03)

Each applied alone, built, run, reverted. The **collateral** column is the point
of recording them together: five of the six redden exactly one test, and the one
that does not says something about the suite.

| Mutation | RED | Collateral, and what it means |
|---|---|---|
| `rebuildProject()` binds a **recomputed** position (its own insertion ordinal) instead of the exported one | ANTS-3796 INV-1 | ANTS-3761 INV-1 and INV-2, plus INV-3's baseline assert. **INV-18 stays green** — a golden compares bytes the writer produced from the *live* store, which a rebuild bug never touches. The golden cannot witness this class at all, which is why INV-1 is an assertion and not another golden |
| `rebuildProject()` never inserts `source_path` | ANTS-3796 INV-2 | none |
| the importer's two guards removed, so a missing `position` defaults to 0 | ANTS-3796 INV-7 | none |
| `sectionOrderLess()` compares `position` alone, no slug tie-break | ANTS-3796 INV-5 | none |
| the loader numbers per source, restarting at 0 for each archive | ANTS-3796 INV-4 (in `roadmap_migrate_load`) | none |
| the derived diff *skips* a foreign-key rowid with no substitution entry rather than failing | ANTS-3796 INV-3 | none |

**The run itself had to be done twice, and the reason is worth more than the
table.** The first harness restored each file with `shutil.copy2`, which
preserves mtime — so a restored file looked *older* than the object compiled
from the mutated one, ninja skipped the rebuild, and every later mutation ran
against a binary still carrying the earlier ones. Three verdicts were measured
against a contaminated build, and the final restore left a tree whose sources
were correct and whose binaries were not. It surfaced only because INV-5
reddened under a mutation it has no code path to. A mutation harness must bust
the mtime on restore, and must re-verify a green tree between mutations; both
are now in the script. This is the same shape as `focused_test` over a stale
binary — the check ran, the check was green, and the check could not see the
failure.

Dropping `provenance` — the column INV-2's clause named — reddens INV-1 as
well, because § 2.4 emits `provenance` always and the reader refuses an export
missing it. That is the reader working correctly, but it means `provenance`
cannot demonstrate INV-2's independence; `layman` can, and does. The first cut
of the reader made this worse still: it silently wrote `''` into a `NOT NULL`
JSON column, producing a store whose *next* export would abort on a row nothing
had reported. The parent's § 2.7 now states the reader's abort obligation.
