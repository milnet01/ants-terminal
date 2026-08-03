# ANTS-3761 — Roadmap export: RFC 8785 serialisation, record types, and the round-trip

**Status:** implemented (2026-07-31) — `src/jsoncanonical.{h,cpp}` and
`src/roadmapexport.{h,cpp}` in `ants_roadmapstore_lib`, behind
`tests/features/roadmap_export_roundtrip/` and
`tests/features/roadmap_export_concurrency/`. All eight invariants green
(loop-log rows 5-impl, 6-impl, 7-impl). § 8's checksum-line question stays open.
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3761 (split out of ANTS-3756 at its cold-eyes cap).
**Blocked by:** [ANTS-3756](ANTS-3756-roadmap-store-schema.md) — the store schema this serialises.
**Pairs with:** [`roadmap-data-model.md`](../standards/roadmap-data-model.md) — its § 1 defines the export as the durable record; this spec defines the bytes.

**Layman:** The plain-text file committed beside the roadmap database that can rebuild it from scratch, and the rules that make it come out identical every time.

## 1. Problem

[`roadmap-data-model.md`](../standards/roadmap-data-model.md) § 1 makes the
export the **durable record**: the store is untracked, the published render is
lossy by design, so the export is the only complete copy that survives a lost
disk. Its INV-1 requires the round-trip to be exact.

"Exact" needs a serialisation nobody can implement two ways.
[ANTS-3756](ANTS-3756-roadmap-store-schema.md) originally carried this and
attempted it with a hand-written table of about twenty pinned freedoms. That
approach did not converge — three cold-eyes loops, with fix collateral
outnumbering draft defects in the last two, almost all of it in this half of
the document. The failure was structural: every freedom pinned by hand is a
rule some other passage can contradict, and the list is never provably
complete. The clinching evidence was external — `QJsonObject` sorts its keys,
so the hand-chosen field order could not be produced by Qt's own JSON classes
at all.

This spec exists because that half is a separate contract with a separate
failure mode, and because delegating canonicalisation to a published standard
is what makes it tractable.

## 2. Surface

### 2.1 File, location, and shape

One file per project, **JSON Lines**, in the private `claude-config` repo at
`roadmap-export/<export_slug>.jsonl`. JSON Lines rather than one JSON document
because a changed item is then a one-line diff, which is the review granularity
ANTS-3753 asked for.

`export_slug` is `project.export_slug` (ANTS-3756 § 2.3), which carries a
`UNIQUE` constraint precisely so two projects cannot overwrite one another's
backup.

### 2.2 Serialisation: RFC 8785, by reference

**Each line is one JSON object serialised per
[RFC 8785](https://www.rfc-editor.org/rfc/rfc8785), the JSON Canonicalization
Scheme (JCS).**

JCS fixes, by reference and **without restatement here**: object key order,
whitespace, string escaping, number formatting, and text encoding. Those five
categories are named so a reader knows what is settled; their *content* is the
RFC's and is deliberately not reproduced, because a second copy is a second
thing to keep in step.

Two consequences an implementer must not discover the hard way:

- **Key order is JCS's, not a hand-chosen reading order.** The record shapes in
  § 2.3 are written readably for humans; the *emitted* order is lexicographic.
  Do not treat the examples as byte-exact.
- **`QJsonDocument::toJson(Compact)` is not JCS — measured, and the gap is
  numbers alone.** Run against the RFC's own material on Qt 6 (§ 8 records the
  run): all **six** published vector files — `arrays`, `french`, `structures`,
  `unicode`, `values`, `weird` — come back byte-identical, so Qt's key order,
  string escaping, whitespace and UTF-8 already agree. Appendix B's number
  table scores **21 of 24**, and all three failures are one root cause:

  | IEEE 754 | RFC 8785 | `QJsonDocument` |
  |---|---|---|
  | `3eb0c6f7a0b5ed8c` | `9.999999999999997e-7` | `9.999999999999997e-07` |
  | `3eb0c6f7a0b5ed8d` | `0.000001` | `1e-06` |
  | `becbf647612f3696` | `-0.0000033333333333333333` | `-3.3333333333333333e-06` |

  ECMAScript switches to exponential notation only outside `1e-6 … 1e21` and
  writes the exponent unpadded; Qt switches earlier and pads to two digits.
  Everything else Qt already gets right, integers past 2^53 included.

  So the writer **canonicalises the number itself** and matches Qt elsewhere,
  rather than vendoring a JCS library for a three-case divergence or trusting
  Qt for a contract it does not claim. Shortest-round-trip digits come from
  `std::to_chars`; ES6's fixed-versus-exponential boundary is applied on top.
  Assuming Qt's output conforms is the obvious mistake, and it is the one that
  made the previous approach unimplementable — INV-19 is what keeps that
  assumption from reappearing.

JCS canonicalises **one JSON value**. It says nothing about a file of them, so
§ 2.4 still owns everything at file level.

### 2.3 Record types

Every line is one JSON object with a `t` discriminator.

```jsonl
{"t":"meta","schema":1,"project":"ants-terminal","name":"Ants Terminal"}
{"t":"id_prefix","prefix":"ants","high_water":3759}
{"t":"legend","status":"in-progress","wording":"In progress (active commit work…)"}
{"t":"section","slug":"performance-2","title":"Performance","level":3,"parent":null,"intro":null}
{"t":"section","slug":"vt-parser","title":"VT parser","level":4,"parent":"performance-2","intro":"Prose."}
{"t":"item","id":"ANTS-1234","id_origin":"parsed","status":"shipped","kind":"perf","headline":"…","layman":"…","source":"…","priority":2,"visibility":"public","milestone":null,"resolution":"…","body":"…","created":"2026-07-30","last_modified":"2026-07-30","shipped":"2026-07-30","lanes":["vt"],"evidence":[],"extras":{},"provenance":{"kind":"defaulted"}}
{"t":"element","section":"performance-2","position":0,"kind":"item","ref":"ants-1234"}
{"t":"element","section":"performance-2","position":1,"kind":"narration","payload":"Prose belonging to no item."}
{"t":"element","section":"performance-2","position":2,"kind":"table","payload":{"header":["A","B"],"rows":[["1","2"]]}}
{"t":"rel","type":"blocked-by","src":"ants-1234","dst":"ants-1200"}
{"t":"rel","type":"blocked-by","src":"ants-1234","dst_project":"doom-ants","dst":"doom-0082"}
{"t":"rel","type":"specified-by","src":"ants-1234","dst_path":"docs/specs/ANTS-1234-thing.md"}
{"t":"citation","project":"ants-terminal","src":"ants-1234","file":"src/vtparser.cpp","symbol":"VtParser::feed"}
{"t":"citation","project":"ants-terminal","doc":"docs/specs/ANTS-1234-thing.md","file":"src/vtparser.cpp","symbol":"VtParser::feed"}
{"t":"feedback_ref","item":"ants-1234","file":"Vestige_Ants_MCP_Feedback.md"}
{"t":"history","item":"ants-1234","at":"2026-07-30T09:15:00Z","seq":0,"field":"status","old":"planned","new":"shipped"}
```

**Every variant is shown, because a variant with no shape is a variant nobody
can export.** `section` appears twice (root and nested), `element` three times
(one per `kind`), `rel` three times (same-project item target, cross-project
item target, document target), `citation` twice (item-anchored and
doc-anchored).

#### What is and is not emitted

- **No surrogate key is ever serialised, and the rule is normative rather than
  the list.** `item_pk`, `project_id`, `section_id`, `element_id`, `rel_id`,
  `citation_id` and `history_id` are rowids, and so is every column *holding*
  one — `section.parent_id`, `element.item_pk`, `relationship.src_pk` /
  `dst_pk`, `history.item_pk`, `citation.item_pk`, `feedback_ref.item_pk`,
  `item.project_id`. (`item.section_id` was on this list until ANTS-3756 § 2.3
  removed the column; the rule below is what makes the list's length
  irrelevant.) **Any column whose value is a rowid is
  never serialised.** A rebuild inserts in *export* order while the source
  store was built in *document* order, and any deleted row leaves a gap that
  never recurs — so a serialised rowid guarantees INV-1 fails. (A list is what
  gets four names short; that is how it stood before the split.)
- **References carry the FOLDED id.** `{"t":"item","id":"ANTS-1234"}` declares
  the item with its authored spelling; every reference to it — `element.ref`,
  `rel.src`/`dst`, `feedback_ref.item`, `citation.src`, `history.item` —
  carries `"ants-1234"`. `id_fold` is never emitted as a *field of its own*;
  it is the reference form. Folding at the reference site is what lets `Sh-1`
  and `SH-1` resolve to one row without the reader re-deriving identity per
  link.
- **Folding is ASCII-lowercase, and the function is named because the two
  obvious choices differ.** SQLite's `lower()` folds ASCII only;
  `QString::toLower()` applies full Unicode case mapping. Ids are ASCII by
  `roadmap-format.md` § 3.5.1's grammar, so the two agree — but an implementer
  must not reach for `toLower()` and assume equivalence.
- **The `item` record carries NO `section`.** An item's filing is its `element`
  record, which names the section and the position together — ANTS-3756 § 2.3
  removed the `item.section_id` column for the same reason it has no
  `sort_order` column, and the export follows the store rather than
  re-introducing the second copy at serialisation time. A rebuild reads the
  `element` line to file the item, which is also why `section` records are
  emitted before `element` records (§ 2.4).
- **`meta` carries no export timestamp.** A date inside a byte-identity
  contract defeats it: two exports of an unchanged store would differ across
  midnight, and every regeneration would churn the committed file.
- **`project.root` is store-local and deliberately unexported** — a
  machine-local absolute path, in a repo shared between machines. It is the one
  column INV-2 does not cover, and ANTS-3756 makes it nullable to say so.
- **A cross-project relationship is carried by the SOURCE project's file**,
  with `dst_project` naming the far side. The alternatives — duplicating the
  edge in both files, or a corpus-level file — were rejected: duplication makes
  one logical edge two rows that can disagree, and a corpus file breaks the
  one-file-per-project rule INV-1 is stated over. A rebuild that cannot see the
  far project keeps the edge and leaves it unresolved; ANTS-3756's `blocked`
  field already excludes cross-project targets for exactly this reason.
- `history.seq` disambiguates two edits to one field within the same second;
  without it the `history` sort is not total.

### 2.4 File-level rules JCS does not cover

| Freedom | Pinned to |
|---|---|
| Record-type order | `meta`, `id_prefix`, `legend`, `section`, `item`, `element`, `rel`, `citation`, `feedback_ref`, `history` — all ten, in that order |
| `id_prefix` order | by `prefix`, code-unit order |
| `legend` order | by `status`, in the model's § 7.3 declared enum order |
| `section` order | by `(depth, slug)` — `depth` counted along the `parent_id` chain, `slug` code-unit order. Depth is what makes **parents before children** a sort *key* rather than a constraint the key has to be checked against; it is not `section.level`, which is the markdown heading level and may skip (`##` then `####`) |
| `item` order | by the id sort in § 2.5 |
| `element` order | by `(section, position)` — `section` by code unit, `position` numeric |
| `rel` order | same-project item targets, then cross-project item targets, then document targets; within each, by `(type, src, dst)` / `(type, src, dst_project, dst)` / `(type, src, dst_path)`, code-unit order |
| `citation` order | item-anchored rows before doc-anchored rows, then by `(src, file, symbol)` / `(doc, file, symbol)`, code-unit order |
| `feedback_ref` order | by `(item, file)`, code-unit order |
| `history` order | by `(item, at, seq)`, strings by code unit — total by construction, since `seq` is unique per `(item, at)` |
| Absent vs null | **per field, from the table below** — not a general rule with a whitelist, which admitted two readings |
| Empty string vs absent | an empty string is a **value** and is emitted; absence is omission |
| Booleans | the format has **no boolean field** — `id_origin` is the enum string `parsed` / `synthesised` / `quarantined`, matching the store's column. Were one added, `true` / `false`, never `0` / `1` |
| Byte-order mark | none. JCS mandates UTF-8 (§ 3.2.4) but says nothing about a BOM, so this spec forbids one explicitly |
| Line ending | `\n`, including after the final line |
| Numbers | integers only, and **JCS § 3.2.2.3 still governs how they render** — it is what forbids `1.0`, `+1` and `1e3`. Values stay inside the 2^53 safe range (§ 3.1.4.3). "No non-integer value in the model" is not licence to hand-roll integer output |
| Timestamps | dates `YYYY-MM-DD`; `history.at` is `YYYY-MM-DDTHH:MM:SSZ` — UTC, second precision, always `Z` |

**Every string comparison in the table above is UTF-16 code-unit order**, the
same collation § 2.5 rule 2 pins for ids — never locale collation, which varies
by `LC_COLLATE` and would make the export machine-dependent.

**That collation is also why the writer sorts in C++ rather than in `ORDER BY`,
and the exception is instructive.** SQLite's default `BINARY` collation is
UTF-8 *byte* order, which equals code-*point* order and therefore disagrees
with UTF-16 code-unit order on any key holding a supplementary-plane character
— those encode as surrogates, which sort *below* U+E000–U+FFFF in UTF-16 and
above it in UTF-8. Section slugs come from headings and roadmap headings carry
emoji, so this is reachable rather than theoretical. The one place `ORDER BY`
*is* used is `history`, whose key is `(changed_at, seq)` within an item —
fixed-width ASCII and an integer, where the two collations provably agree. That
exception is what lets the one unbounded table stream (INV-12) instead of
having its keys collected.

#### Absent, null, or empty — per field

A general "omit when absent" rule plus a null whitelist admitted two readings:
that the whitelisted fields are *always* emitted, or that the omit rule wins and
they never are. Both are defensible from the sentence, and they disagree on the
commonest field state in the corpus. So the rule is per field:

| Field | When it has no value |
|---|---|
| `section.parent`, `section.intro`, `item.milestone` | **always emitted**, as `null` |
| `item.layman`, `item.priority`, `item.resolution`, `item.body`, `item.created`, `item.last_modified`, `item.shipped` | **omitted** |
| `item.source` | **never has no value** — ANTS-3756 § 2.3 makes the column `NOT NULL` and the model's § 3.3 gives migration a default, so it is always emitted. Listed here because an earlier draft filed it under *omitted*, which is a state the store cannot produce; a writer implementing that branch would be writing dead code against a `NOT NULL` column |
| `history.old`, `history.new` | **omitted**. Both columns are nullable and the first revision of any field has no `old`, so this is the commonest null in the table — and this table is exhaustive by construction, so leaving them out was a gap a writer would have had to guess at |
| `item.lanes`, `item.evidence`, `item.extras`, `item.provenance` | **always emitted**, as `[]` / `{}`. ANTS-3756 makes these columns `NOT NULL DEFAULT '[]'`/`'{}'` so NULL and empty are not distinct states at rest |
| `legend` records | a project with no legend emits **zero** `legend` lines; the rebuild restores `{}`, matching the store's default. A legend key *outside* § 7.3's enum has no position in the declared order, so no rule would emit it — the export **aborts** rather than dropping a store row silently (the same disposal as the JCS failures below) |

#### When a row cannot be serialised

RFC 8785 requires a conforming implementation to **terminate with an error** on
invalid UTF-16 (§ 3.2.2.2) and forbids duplicate object keys (§ 3.1) — both
reachable here, since SQLite `TEXT` does not validate encoding and migrated
`extras` come from hand-written markdown. **The export aborts and reports the
offending row; it never emits a partial file and never substitutes a
replacement character.** A truncated backup that looks complete is the failure
this whole spec exists to prevent.

**Partitioning each multi-variant type before sorting it is what makes those
orders total.** On a document-target `rel` the `dst` member is *absent*, and
nothing in JSON defines how an absent member collates against a present one —
so a single sort key over `(type, src, dst, dst_path)` is undefined for exactly
the rows that mix variants. Ordering by variant first removes the comparison
rather than defining it.

**Sections are emitted parents-before-children** so a single-pass rebuild can
resolve `parent` as it reads. Slug order alone can emit a child first, which
would force the reader to defer foreign keys or make two passes.

### 2.5 Id sort order

Items sort by a **variable-length numeric-segment tuple**, because no simpler
rule survives the corpus:

1. Fold the id, then **remove every separator (`-`, `_`)**, then split the remaining text into maximal **alphabetic** and **numeric** runs. Removal happens *before* splitting — that is the whole rule, and the ordering of the two steps is the part an implementer gets wrong.
   - `ants-1234` → `ants1234` → `("ants", 1234)`
   - `pass-43-5` → `pass435` → `("pass", 435)`
   - `3d_e-0007` → `3de0007` → `(3, "de", 7)` — note it **starts numeric**
   - `3de-0007` → `3de0007` → `(3, "de", 7)` — **identical**, which is the point

   An earlier draft said separators were "discarded, not compared" but split on
   them anyway, yielding `(3,"d","e",7)` for the first and `(3,"de",7)` for the
   second — which sort *differently*, exactly the outcome the sentence claimed
   to prevent. Remove-then-split is what makes the claim true. The raw-id
   tie-break in rule 5 keeps the two distinguishable.
2. Compare run by run. Two runs of the same type compare naturally: numeric by value, alphabetic by **UTF-16 code-unit order** on the folded text — never locale collation, which varies by `LC_COLLATE` and would make the export machine-dependent.
3. **Where two runs differ in type, numeric sorts before alphabetic.** Not cosmetic: `3D_E` is a live prefix, so `3d_e-0007` begins with a numeric run where `ants-1234` begins with an alphabetic one, and without this rule the sort is undefined between two real projects' ids.
4. A shorter tuple sorts before a longer one sharing its prefix (`pass-43-5` before `pass-43-5-b`).
5. **Tie-break on the raw `id`, code-unit order.** Required, not defensive: zero-padding is write-side only (`roadmap-format.md` § 3.5.1), so `CL-9` and `CL-0009` fold to the identical tuple and the sort would otherwise be non-total — which makes INV-1 fail *intermittently*, the worst way for it to fail.
6. Only `id_origin = 'quarantined'` ids (ANTS-3756 § 2.3) sort **last**, among themselves by raw `id`. `parsed` and `synthesised` ids both use the tuple sort above — a synthesised `PASS-43-5-B` is a real id the model's § 7.1 recognises, and sorting it with the junk would bury 144 live items.

Lexical sorting is not available (`ANTS-10` before `ANTS-9`), and neither is
zero-padded string sorting (the corpus has genuinely unpadded ids: `CL-9`
alongside `ANTS-0001`).

**The variable-length tuple is justified by `PASS-43-5`, not by
`PASS-43-5-B`** — and the distinction matters because an earlier draft got it
backwards. `PASS-43-5-B` does **not** match `roadmap-format.md` § 3.5.1's
grammar, which requires an id to end in `-<digits>`; it reaches the sort only
because ANTS-3756 marks synthesised ids as such rather than quarantining them.
`PASS-43-5` *does* match, and it is what defeats a two-element
`(prefix, numeric)` tuple: splitting at the last hyphen makes the prefix
`pass-43`, so `PASS-9-1` would sort after `PASS-43-5`. Remove-then-split gives
`(pass, 435)` and `(pass, 91)`, ordering them correctly without a special case.

### 2.6 The write path

- **Read inside one deferred transaction.** The export spans many statements; without a transaction a commit landing mid-export tears the file — half pre-change, half post-change, and INV-1 fails against a store nobody corrupted.
- **`ConfigWriteLock`** (`src/configbackup.h`) wraps the write. It is the project's existing RAII `flock(2)` guard, and `tests/features/concurrent_writer_lock/` already locks its behaviour; the export is a whole-file rewrite, exactly the read-modify-write shape it exists for. Reusing it beats a second locking scheme (`coding.md` — reuse before rewriting).
- **On a failed acquire the export ABORTS and reports.** The guard is advisory and its header leaves the choice to the caller. Proceeding unprotected is not available here: the model's § 9 says a silent backup failure is worse than no backup, because it stops anyone checking.
- **Written temp-then-`rename(2)`, inside the lock's scope.** A crash midway through an in-place write truncates the only durable copy of a primary store — the one outcome worse than not having written it. `QSaveFile` is the project's existing form of exactly that, so it is what the writer uses. (`JsonlFile::writeLinesAtomic` is the closer match by *shape* and is deliberately not reused: it takes every line in memory at once, which is what INV-12 forbids.)

### 2.7 The rebuild

**This spec owes a reader as well as a writer, and an earlier draft named only
the writer.** INV-1 and INV-2 are both stated over export → **rebuild** →
re-export, so a spec with no rebuild in it states two invariants nothing can
satisfy. It is not deferred work either: § 5 sends *migration from markdown* to
ANTS-3757, which is a different job — that one parses prose, this one reads a
file this spec's own writer produced. Added at implementation (row 6-impl).

- **Single pass, no deferred references.** § 2.4's record order guarantees
  every reference is declared before it is used — that is *why* `section`
  precedes `element` and `item` precedes both, and why sections are emitted
  parents-first. A reader that needed two passes would mean the order was
  bought for nothing.
- **Inside one `BEGIN IMMEDIATE`,** never plain `BEGIN` (ANTS-3756 § 2.5): a
  deferred transaction that reads then writes must upgrade, and SQLite returns
  `SQLITE_BUSY` on that upgrade without honouring `busy_timeout`.
- **`project.root` is restored as NULL**, which is the whole reason ANTS-3756
  makes the column nullable: the path is machine-local and § 2.3 never exports
  it.
- **`history` rows are inserted directly, NOT through the append path.** That
  path enforces ANTS-3756's INV-14 cap because it is adding a *revision*; a
  rebuild is restoring rows that were already inside the cap when written.
  Refusing them would make a store at its bound unrebuildable from its own
  backup — the one situation the backup exists for.
- **The reader aborts and reports exactly as the writer does.** § 2.4's "never
  a partial file, never a substitution" is a property of the round trip, not of
  one direction: a reader that patches a malformed record writes a store whose
  *next* export fails, on a row nothing ever reported. Found by mutation
  (row 6-impl) — the first implementation silently wrote `''` into a `NOT NULL`
  JSON column when a key was missing.

## 3. Invariants

Numbers are **inherited from ANTS-3756 and deliberately not reflowed**
(`specs.md` § 5.5 — invariant ids are permanent). The gaps are the invariants
that stayed with the store.

- **INV-1** — Export, rebuild from that export, re-export ⇒ byte-identical files. This holds **per project and across the whole corpus** — a corpus-wide rebuild must also preserve the cross-project relationships the model's INV-4 allows, which a per-project round-trip cannot witness. *Test:* `tests/features/roadmap_export_roundtrip/` builds a **synthetic three-project fixture** — never the machine's real corpus, which is not present in CI — seeded with two items whose insertion order differs from their id order, a deleted row (so rowids carry a gap), a nested section, and one cross-project `blocked-by`. Export all three, rebuild into a temp store, re-export, `cmp` each pair, and assert the cross-project edge survives on the source project's file. *Breaks when:* any § 2.4 rule is left unpinned.
- **INV-2** — The export is complete: every store row, and every **non-surrogate** column of it, survives the round-trip. *Test:* `roadmap_export_roundtrip` — after rebuild, per-table `COUNT(*)` matches for all **nine** tables (ten *record types*; `legend` is a column on `project`, not a table), and a column-wise diff matches, **joining on stable identity**: `export_slug` for the project row, `(export_slug, id_fold)` for items, `slug` for sections, `(section, position)` for elements, `(type, src, dst_project, dst|dst_path)` for relationships — `dst_project` is part of the key, because ANTS-3756's `rel_xproj_uq` leads with it and two projects can hold the same folded id, so a key omitting it merges a cross-project edge with a same-project one — `(item, at, seq)` for history, `(project, src|doc, file, symbol)` for citations — `project` likewise leads `cite_doc_uq`, and two projects each citing their own `README.md` are two rows — `(item, file)` for feedback refs, `prefix` for id prefixes. Every rowid-valued column and `project.root` are excluded. *Breaks when:* a writer drops a whole column that § 2.4 **omits when absent** — `layman`, say. The drop round-trips byte-identically and preserves every row count, so INV-1 and a count-only check both pass on a lossy store; only the column diff sees it. **The column class matters, and an earlier draft named the wrong one:** dropping `provenance` reddens INV-1 as well, because § 2.4 emits it always and the reader therefore refuses an export missing it — measured (row 6-impl). That is the reader working correctly, but it means `provenance` cannot demonstrate what this invariant is for. The surrogate exclusion is not a weakening: § 2.3 guarantees rowids differ after a rebuild, so a diff including them fails against a *correct* implementation.
- **INV-3** — *moved to ANTS-3756* (item identity folding) — see [ANTS-3756](ANTS-3756-roadmap-store-schema.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-4** — *moved to ANTS-3756* (off-grammar id storage) — see [ANTS-3756](ANTS-3756-roadmap-store-schema.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-5** — Export item order follows § 2.5's numeric-segment sort and is **total**. *Test:* `roadmap_export_roundtrip` seeds `ANTS-9`, `ANTS-10`, `CL-9`, `CL-0009`, `PASS-43-5`, `PASS-43-5-B`, `PASS-9-1`, `3D_E-0007`, `3DE-0007` and one quarantined id, then asserts the exact emitted order. The last two are not padding: `3D_E-0007` is the only seed beginning with a *numeric* run, so it is what exercises rule 3, and the pair together exercises rule 1's separator-discard — a writer implementing neither passes a seed set without them. *Breaks when:* the writer sorts lexically (`ANTS-10` before `ANTS-9`); or splits at the last hyphen (`PASS-9-1` after `PASS-43-5`); or omits the tie-break, leaving `CL-9` and `CL-0009` unordered.
- **INV-6** — *moved to ANTS-3756* (relates-to stored once) — see [ANTS-3756](ANTS-3756-roadmap-store-schema.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-7** — *moved to ANTS-3756* (store location) — see [ANTS-3756](ANTS-3756-roadmap-store-schema.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-8** — *moved to ANTS-3756* (canonical project root) — see [ANTS-3756](ANTS-3756-roadmap-store-schema.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-9** — Every export write path acquires `ConfigWriteLock`, and aborts loudly when it cannot. *Test:* `roadmap_export_concurrency` — hold the lock, attempt an export, assert it returns an error, **wrote no bytes**, and **left no temp file**; release, assert it then succeeds. *Breaks when:* the writer treats `!acquired()` as permission to proceed unprotected. Phrasing matters: `flock` is advisory, so a non-cooperating writer *can* interleave — the testable claim is about our writer, not about the file.
- **INV-10** — *moved to ANTS-3756* (per-field provenance) — see [ANTS-3756](ANTS-3756-roadmap-store-schema.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-11** — *moved to ANTS-3756* (enum enforcement at the storage layer) — see [ANTS-3756](ANTS-3756-roadmap-store-schema.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-12** — The export writer streams: peak RSS during an export at corpus scale rises by **less than 4 MiB** above the pre-export baseline, against an export several times that size. *Test:* `roadmap_export_roundtrip` inflates the synthetic fixture to ~5 MiB of generated body text, samples RSS from a **watcher thread at 10 ms intervals** across the export call — an export is synchronous, so "peak" needs a sampler; reading RSS before and after would miss it entirely — and asserts the **delta** between the pre-call baseline and the observed peak. *Breaks when:* the writer builds one `QString` and writes it at the end — which passes every other invariant here. The measurement must be a delta: a Qt process's absolute RSS exceeds the export's byte size before any work is done, so an absolute ceiling is unachievable and would be quietly relaxed until it passed. It must also run in an **unsanitized** build, which is a precondition of the instrument and not a tolerance to widen: ASan's redzones and free quarantine make process RSS describe the sanitizer rather than the writer, measured at a 120 MiB delta for the same streaming writer that passes in Release. The test `GTEST_SKIP`s under ASan and the Release build holds the invariant.
- **INV-13** — Every cross-record reference resolves to a declared stable key, and no surrogate value is emitted under any name. *Test:* `roadmap_export_roundtrip` parses the export and asserts every `ref` / `src` / `dst` / `item` / `section` / `parent` / `doc` value resolves — **folding before comparison**, since references are folded and `item.id` is authored. *Breaks when:* a writer serialises straight from `SELECT *` — the natural implementation, which silently breaks INV-1 on any store that has ever deleted a row. A name-based grep for `item_pk` is **not** sufficient: it passes against a writer emitting the same rowids under a different key (`"section":3`), and false-positives on free-text `body` or `extras`.
- **INV-18** — The fixture's export matches a **committed golden file**, byte for byte. *Test:* `roadmap_export_roundtrip` compares its export against `tests/features/roadmap_export_roundtrip/golden/*.jsonl`, which are checked in and reviewed. *Breaks when:* the writer ignores § 2.4 wholesale — INV-1 compares the writer against *itself*, so **any deterministic writer passes it**, including one that emits records in insertion order. Self-consistency is not conformance; this is the invariant that makes the file-level rules testable at all. Regenerating a golden file is a reviewable diff and must never be done to make a test pass.
- **INV-19** — The serialiser conforms to RFC 8785 on the RFC's own test vectors. *Test:* `roadmap_export_roundtrip` runs the published JCS test-vector set through the writer's canonicalisation entry point and asserts exact output. *Breaks when:* the writer uses `QJsonDocument::toJson(Compact)` and assumes it conforms — § 2.2 warns about this, and a warning with no test behind it is a comment.
- **INV-14** — *moved to ANTS-3756* (history bounded) — see [ANTS-3756](ANTS-3756-roadmap-store-schema.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-15** — *moved to ANTS-3756* (store creation race) — see [ANTS-3756](ANTS-3756-roadmap-store-schema.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-16** — *moved to ANTS-3756* (busy-timeout write policy) — see [ANTS-3756](ANTS-3756-roadmap-store-schema.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-17** — *moved to ANTS-3756* (store file mode 0600) — see [ANTS-3756](ANTS-3756-roadmap-store-schema.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.

## 4. RAM / build cost

**Memory.** The export writer streams (INV-12), so its budget is a fixed
per-record working set rather than a function of corpus size — under 4 MiB
above baseline for a corpus whose markdown is 4.91 MiB (measured; ANTS-3756
§ 1 records the command). The rebuild reads line by line for the same reason.

**Disk.** The export is comparable to the store's own size, order 5–10 MiB for
the current corpus, dominated by `body` text and `history`. `history`'s bound is
ANTS-3756's INV-14, now settled as **250 MB across the whole store** with
nothing evicted below it — so the export's own worst case is bounded by that
figure rather than by anything this spec sets. That is a large ceiling for a
git-committed file, and deliberately so: the model's § 6 makes the export the
only place history survives, and a tighter bound would be this spec quietly
discarding what it exists to preserve. If the corpus ever approaches it, the
decision to make is the store's eviction rule, not a second cap here.

**Build.** No new target and no new dependency — the export writer lives in
ANTS-3756's `roadmapstore` library and needs `Qt6::Core` only. The JCS
requirement adds no third-party code: § 2.2's measurement shows the divergence
is confined to number rendering, so the writer canonicalises numbers with
`std::to_chars` (`<charconv>`, already available at the project's C++20 floor)
and no JCS library is vendored. § 8's first open question is closed on that
evidence.

## 5. Out of scope

- **The store schema, its constraints, location and connection pragmas.** [ANTS-3756](ANTS-3756-roadmap-store-schema.md).
- **Migration — populating the store from the ten markdown roadmaps.** ANTS-3757.
- **The auto-publish cadence to the backup repo, and what a push conflict means.** ANTS-3758. This spec produces the bytes; when and how often they are committed is that spec's.
- **How the writer locates the `claude-config` checkout.** ANTS-3758, with the rest of the backup surface. A **permanent exclusion here** rather than deferred work in this spec: the path is a deployment concern, not a serialisation one.
- **Compression or a binary format.** Permanent exclusion — the whole point of the export is a git-reviewable one-line-per-item diff, which both would destroy.

## 6. Tests

Feature tests under `tests/features/`, label `features;fast`:

| Directory | Covers |
|---|---|
| `roadmap_export_roundtrip/` | INV-1, INV-2, INV-5, INV-12, INV-13, INV-18, INV-19 |
| `roadmap_export_concurrency/` | INV-9 |

All eight invariants are covered; none is a grep-only check. The bundle ships
`golden/` (INV-18) and the RFC's test vectors (INV-19) as committed fixtures.

Per `CLAUDE.md` and `testing.md`, each test must be verified to **fail against
pre-implementation source** before the implementation is restored.

**For INV-1 this section used to prescribe the wrong mutation, and measurement
said so** (row 6-impl). It read: "emit one object via a non-JCS path and
confirm the round-trip comparison fails." Run, that mutation leaves INV-1
**green**. `QJsonDocument::toJson(Compact)` is deterministic and its output
re-parses to the same doubles, so the export, the rebuild and the re-export all
agree — INV-18 is what goes red. The instruction was self-refuting: it asked
INV-1 to catch a *deterministic* writer, which is precisely the thing INV-18
was added at loop 1 because INV-1 cannot catch.

What does redden INV-1 is a writer whose output the rebuild cannot reproduce:
emit a surrogate (`"section": <rowid>`) and the re-export differs, because the
fixture's deleted row leaves a rowid gap a rebuild never recreates. That is the
hazard § 2.3 states, and it is the mutation to use.

## 7. Cross-doc impact

- **[ANTS-3756](ANTS-3756-roadmap-store-schema.md)** — its § 2.4 is replaced by a pointer here; **eight** invariants are tombstoned there: INV-1, 2, 5, 9, 12, 13 (moved at the split) plus INV-18 and INV-19 (added here at loop 1, and tombstoned there so the shared numbering stays unambiguous). Both documents must say eight; an earlier six on each side was the same miscount twice.
- **[`roadmap-data-model.md`](../standards/roadmap-data-model.md)** — its § 9 assigns "the export: field order, sort collation, encoding, and every other rule that makes INV-1's 'identical file' testable" to a spec; that bullet points here once this ships.
- **`CHANGELOG.md`** — user-invisible until ANTS-3758 lands the render.

## 8. Open questions

- ~~**Does a JCS implementation exist that is worth vendoring, or is validating `QJsonDocument` against the RFC test vectors cheaper?**~~ **Closed 2026-07-30 by measurement, before implementation** — neither. `QJsonDocument::toJson(Compact)` was run against the RFC's own material: 6/6 published vector files byte-identical, 21/24 of Appendix B's number table, with all three misses on ES6's fixed-versus-exponential boundary. A vendored library would replace a whole conforming serialiser to fix three number cases, and trusting Qt would ship a contract it does not claim; the writer canonicalises numbers itself and keeps Qt's agreement elsewhere. § 2.2 carries the evidence, § 4 the (nil) build cost.
- **Should the export carry a trailing checksum line?** It would make a truncated file detectable without a full parse. Against: it is a value derived from the rest of the file, so it must be excluded from the round-trip comparison, which is the class of exception that caused trouble in the parent spec.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 0-split | 2026-07-30 | none — no reviewer dispatched | — | **Provenance row, not a review.** Split out of ANTS-3756 after that spec converged by cap at 3 loops with fix collateral outnumbering draft defects in the last two, almost all of it in this half. Invariant numbers inherited, not reflowed (`specs.md` § 5.5). Three findings from ANTS-3760's deferred tail are folded in here as the split's owner: the cross-project relationship carrier, the nested-`section` record shape with parents-before-children ordering, and `dst_path`/`doc` naming. **The parent's three loops do NOT transfer** — they were run against a document that no longer exists; this spec runs the rule-14 gate from loop 1 on its own bytes. |
| 1 | 2026-07-30 | 1 (export lane, cold, counterpart also read) | 5 / 5 / 6 / 6 / 1 | First gate on the post-split document. The lane fetched RFC 8785 and confirmed § 2.2's five delegated categories are genuinely JCS's — the split's central bet holds. Three defects it did not: **§ 2.5's separator rule contradicted its own example and rationale** (it claimed `3D_E-0007` and `3DE-0007` sort alike while splitting on separators, which makes them differ) — now remove-then-split, with the ordering of the two steps called out as the part implementers get wrong. **`PASS-43-5-B` does not match `roadmap-format.md` § 3.5.1's grammar**, so the example motivating the variable-length tuple never reached the sort; the justification is rebased onto `PASS-43-5`, which does match and does defeat a two-element tuple. **INV-1 tested the writer against itself** — any deterministic writer passes it, including one ignoring § 2.4 entirely — so INV-18 (committed golden files) and INV-19 (the RFC's own test vectors) now make conformance testable. Also: absent/null/empty is a per-field table rather than a rule plus whitelist that admitted two readings; a JCS failure mode (abort, never a partial file); code-unit collation stated on every ordering row; no BOM; and the number row corrected — JCS § 3.2.2.3 governs integer rendering too. |
| 2-fold | 2026-07-30 | none — no reviewer dispatched | — | **Decision row, not a review.** Rows are ordered ascending from here, matching ANTS-3756 and `roadmap-data-model.md`; the two existing rows were previously logged newest-first, which made the split look like it followed the gate it preceded. ANTS-3756's INV-14 is now settled (store-wide 250 MB, nothing evicted below it), so § 4's "this spec inherits whatever that settles" is replaced by the figure and by *why* this spec does not add a second, tighter cap of its own: the model's § 6 makes the export the only place history survives, so bounding it here would discard exactly what it exists to preserve. No invariant changed. |
| 3-seam | 2026-07-30 | none dispatched here — findings arrived from ANTS-3756's loop 5 | — | **Seam fixes, not a review of this document.** Two cold lanes reading ANTS-3756 also read this file as its cross-reference and found six defects on the boundary; they are fixed here because this is the side that owns them. Both documents said **six** invariants were tombstoned in ANTS-3756 where there are **eight** (INV-18 and INV-19, added at loop 1 here, were tombstoned there and left out of both counts). § 2.4's absent/null table is exhaustive by construction, and omitted `history.old` / `history.new` — the commonest null in the file, since no field's first revision has an `old` — while filing `item.source` under *omitted*, a state ANTS-3756's `NOT NULL` column cannot produce. INV-2's join keys omitted `dst_project` and citation `project`, both of which **lead** the store's own unique indexes, so the diff would have merged a cross-project edge with a same-project one and fused two projects' identically-named doc citations. § 4's inherited `history` bound is now the settled figure. This document has **not** had a cold read since these edits; its next gate run is against changed bytes. |
| 4-measure | 2026-07-30 | none — a measurement, not a review | — | **Decision row.** § 8's first open question — vendor a JCS implementation, or validate `QJsonDocument` against the RFC's vectors — closed **before** implementation by running the question rather than arguing it. The six published JCS vector files (`arrays`, `french`, `structures`, `unicode`, `values`, `weird`) and RFC 8785 Appendix B's 24-row number table were put through `QJsonDocument::toJson(Compact)` on Qt 6: **6/6 files byte-identical, 21/24 numbers**. Every miss is the same root cause — ECMAScript leaves fixed notation only outside `1e-6 … 1e21` and writes the exponent unpadded, where Qt switches earlier and pads to two digits (`0.000001` → `1e-06`; `9.999999999999997e-7` → `…e-07`). Key order, escaping, whitespace, UTF-8 and integers beyond 2^53 already agree. **Answer: neither option.** Vendoring replaces a serialiser that already conforms in order to fix three number cases; trusting Qt ships a contract Qt does not claim. The writer canonicalises numbers itself over `std::to_chars` and keeps Qt's agreement elsewhere, so § 4's build cost stays at no new dependency. No invariant changed — INV-19 was already the test, and it now has a known failure mode to catch rather than a suspicion. |
| 5-impl | 2026-07-30 | none — implementation, not a review | — | **Implementation row (canonicaliser), written by the implementer.** `src/jsoncanonical.{h,cpp}` joins `ants_roadmapstore_lib` — ~180 lines, `Qt6::Core` and `<charconv>` only, as row 4-measure predicted. INV-19 lands in `tests/features/roadmap_export_roundtrip/` (the directory § 6 assigns it) in four legs: the six published vector files, Appendix B's 24 numbers addressed by IEEE 754 bit pattern, the RFC's mandatory lone-surrogate abort, and JCS key order. All green; the six files are committed under `vectors/` with provenance and licence. **The leg split earned itself immediately.** Under the mutation that makes `numberToString()` delegate to `QJsonDocument::toJson(Compact)` — the exact mistake § 2.2 names — the six vector files stay **GREEN** and only Appendix B goes RED. A test built from the published files alone would have certified the writer this spec exists to rule out. That is INV-18's argument one level down: self-consistency is not conformance, and neither is *partial* external agreement. Two further mutations went RED as expected (surrogate check removed; key comparator reversed). **One defect fixed outside this spec:** ANTS-3756 § 2.3 requires the store's JSON columns to be *held* in canonical form so the export copies bytes rather than transforms them, and `RoadmapStore::canonicalJson()` was implemented over `QJsonDocument` on the reasoning that sorted keys plus compact output is JCS "for the shapes this store writes". True of every column except `extras`, which the model's § 7.7 makes free-form and which can therefore hold a double. It now calls `JsonCanonical::serialise()`. The clause was always right; the code did not meet it, and only a number-level test could tell. |
| 6-impl | 2026-07-31 | none — implementation, not a review | — | **Implementation row (the export writer), written by the implementer.** `src/roadmapexport.{h,cpp}` joins `ants_roadmapstore_lib`; INV-1, 2, 5, 12, 13 and 18 land in `roadmap_export_roundtrip/` and INV-9 in the new `roadmap_export_concurrency/`, both in the existing `test_core` bundle. All seven green, and **each proved RED first under the exact mutation its own "Breaks when" clause names** — nine mutations, each built and run in isolation. Three findings the run produced that reading would not have. **§ 6's prescribed INV-1 mutation is self-refuting.** It asked for a non-JCS emission; run, INV-1 stays **green** — `QJsonDocument::toJson(Compact)` is deterministic and re-parses to the same doubles, so writer, rebuild and re-export all agree, and INV-18 is what reddens. The instruction asked INV-1 to catch a deterministic writer, which is exactly what INV-18 exists because INV-1 cannot do. § 6 now names the mutation that does work (emit a rowid; the fixture's deleted row leaves a gap no rebuild recreates). **INV-2 named the wrong column class.** Dropping `provenance` reddens INV-1 too, because § 2.4 emits it always and the reader refuses an export missing it; only an *omitted-when-absent* column (`layman`) leaves INV-1 green and INV-2 red, which is the demonstration the invariant needs. **The spec had no reader at all** — INV-1 and INV-2 are both stated over a rebuild that § 2.4/2.6 never described, and § 5's deferral covers migration-from-markdown, a different job. § 2.7 now specifies it, including the two rules mutation exposed: the reader owes the same abort-and-report the writer does (the first cut silently wrote `''` into a `NOT NULL` JSON column), and `history` restores by direct insert so a store at INV-14's bound stays rebuildable from its own backup. Two smaller amendments: § 2.4's `section` row is now the sort key `(depth, slug)` rather than a constraint plus a key, with depth walked from `parent_id` and explicitly not `level`; and the collation note now records why ordering is done in C++ rather than `ORDER BY` — SQLite's `BINARY` is UTF-8 byte order, which disagrees with UTF-16 code-unit order on supplementary-plane characters, reachable via emoji in heading slugs. `history` is the one `ORDER BY`, because its key is fixed-width ASCII where the two provably agree, and that exception is what lets the unbounded table stream. INV-12 measured comfortably inside budget with the `Bulk` profile's 16 MiB page cache against a 5 MiB export, so ANTS-3756 § 2.5's cache figure and this spec's RSS budget do not in fact collide. |
| 7-impl | 2026-07-31 | none — implementation, not a review | — | **Implementation row (INV-12's build precondition), written by the implementer.** CI run 30587366963's `build-asan` job went red on `Inv12PeakRssDeltaStaysUnderFourMiB` while `build-test` stayed green, and the failure reproduced locally at a **120 MiB** delta against the 4 MiB budget — 30× over, for the same writer that passes in Release by a wide margin. The writer had not regressed; the **instrument** had. ASan replaces the allocator: every allocation gains redzones and every free goes to a quarantine rather than back to the OS, so `/proc/self/statm` reports the sanitizer's bookkeeping and a streaming writer is indistinguishable from a buffering one. Widening the budget to fit would have left nothing for a non-streaming writer to exceed, which is the "quietly relaxed until it passed" failure INV-12's own text warns about — so the budget stands and the test `GTEST_SKIP`s under ASan instead, with the Release build (ci.yml `build-test`, and every local preset) holding the invariant. INV-12's *Test:* clause now states the unsanitized-build precondition, which it had left implicit; that omission is the actual spec defect this found. |
