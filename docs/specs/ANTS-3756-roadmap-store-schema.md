# ANTS-3756 — Roadmap store: schema, export serialisation, and the INV-1 round-trip

**Status:** spec draft (2026-07-30).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3756 (ANTS-3753 split, spec seam 1 of 3).
**Blocker for:** ANTS-3757 (migration), ANTS-3758 (publish + consumer cutover).
**Pairs with:** [`roadmap-data-model.md`](../standards/roadmap-data-model.md) — that standard defines *what an item is*; this spec defines how it is stored and serialised.

**Layman:** The database that holds every project's to-do items, and the plain-text file committed alongside it that can rebuild the database from scratch if it is ever lost.

## 1. Problem

[`roadmap-data-model.md`](../standards/roadmap-data-model.md) defines three
artifacts — store, export, published render — and § 9 of that standard
deliberately stops before the schema. Nothing yet says what a table looks like,
what makes the export's "byte-identical" testable, or how two Ants processes
sharing one store avoid corrupting it.

The store is **primary**, not a cache. That distinction was vetoed into place by
the user (ANTS-3753) and it drives most decisions here: the existing derived
caches — `codebase_index` (ANTS-1637), `docs_index` (ANTS-2139) — are JSON
files under `~/.cache/ants-terminal/` that may be deleted at any time and
rebuilt from source. This store has no source to rebuild from except its own
export, so it may not live under a cache path and may not be treated as
disposable.

Scale, measured 2026-07-30 with `tools/roadmap-corpus-survey.py`: **10
projects, roughly 3,900 bullet-form and checkbox items plus ~150 pass-heading
items**, and **4.91 MiB** of markdown across the ten roadmap files
(`find . -maxdepth 2 -iname 'roadmap.md' -printf '%s\n' | awk '{s+=$1} END {print s}'`,
run from the projects' parent — **case-insensitively**, since one project's
file is lowercase; see § 4). Orders of magnitude, not exact counts: the corpus
grows whenever anyone files an item, so re-run the script rather than trusting
a figure here.

## 2. Surface

### 2.1 Storage engine and the dependency it costs

SQLite via **`Qt6::Sql`** with the `QSQLITE` driver.

This is a real addition and is stated rather than glossed. `CMakeLists.txt`
currently declares:

```cmake
find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Network OpenGL OpenGLWidgets DBus Test)
```

`Sql` is not among them. Adding it means:

- `Sql` appended to that `COMPONENTS` list, and `Qt6::Sql` linked by the new
  `roadmapstore` library only.
- The AppImage must bundle the driver plugin (`sqldrivers/libqsqlite.so`); a
  Qt SQL build with no driver fails at `QSqlDatabase::addDatabase` and not at
  link time, so this is a packaging step that a green build will not catch.

**A third-party runtime library DOES enter the graph, and "Qt6 is the only
runtime dep" (`CLAUDE.md`) needs amending rather than defending.** Measured on
this host:

```
$ ldd /usr/lib64/qt6/plugins/sqldrivers/libqsqlite.so | grep sqlite
        libsqlite3.so.0 => .../libsqlite3.so.0
$ rpm -qf /usr/lib64/qt6/plugins/sqldrivers/libqsqlite.so
        qt6-sql-sqlite-6.11.1-1.3.x86_64
```

Distro Qt builds configure `-system-sqlite`, so the driver links the system
`libsqlite3` and ships in its **own package**, separate from Qt Base. Qt does
vendor an amalgamation for builds configured without `-system-sqlite`, but that
is not what any carrier here consumes, and a spec may not pick the reading that
happens to be convenient.

Consequences, all of which § 7 must carry: every packaging carrier gains a
build **and** runtime dependency; the AppImage bundles `libsqlite3.so.0`
alongside the plugin, not just the plugin; and CI's apt list gains the SQL
module or every feature test below fails at runtime on a green build.

The alternative — Qt's `-system-sqlite`-free amalgamation, or vendoring SQLite
ourselves — is rejected: vendoring a C library into a Qt app to preserve a
one-line claim in `CLAUDE.md` costs more than amending the claim.

**Why the MCP helper libraries stay `Qt6::Core`-only.** Sixteen helper
libraries in `CMakeLists.txt` carry an explicit `Qt6::Core-only` comment
(`grep -c "Qt6::Core-only" CMakeLists.txt` → 16; `read_region`,
`codebase_index`, `docs_index`, …) so the test bundles link without GUI or
extra modules. The store does **not** go in any of them: it is its own
`roadmapstore` library, and the pure helpers keep their current link surface.

### 2.2 Location

```
${XDG_DATA_HOME:-$HOME/.local/share}/ants-terminal/roadmap.sqlite
```

Resolved in code as `QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/ants-terminal"`.

**`GenericDataLocation`, never `AppDataLocation`.** `src/main.cpp` calls
`app.setApplicationName("Ants Terminal")` and there is no `setOrganizationName`
anywhere in `src/`, so `AppDataLocation` resolves to
`~/.local/share/Ants Terminal` — a different directory, with a space in it.
Every existing writer in the project (`src/debuglog.cpp`,
`src/sessionmanager.cpp`, `src/terminalgrid.cpp`, `src/terminalwidget.cpp`,
`src/mainwindow.cpp`) uses `GenericDataLocation + "/ants-terminal"`, and the
store follows them.

`XDG_DATA_HOME`, **not** `XDG_CACHE_HOME`. `mcp-caches.md`'s inventory is a
list of things that may be deleted and rebuilt; this file is not one, and it is
added to that document as an explicit non-cache entry (§ 7) so nobody adds it
to a GC sweep later.

The store holds every project, so it is **not** path-keyed and
`mcp-caches.md`'s shadow rule does not apply to the file itself. It applies to
the `project` rows inside it — see INV-8.

### 2.3 Schema

One table per entity, all projects in each. "One table per project", as
ANTS-3753 originally asked, is implemented as **one table keyed on project**;
the reconciliation is recorded in that bullet (nine-way stitches and nine-way
migrations fight the stated goal of maintaining the whole thing from any
project).

`PRAGMA user_version` carries the schema version, starting at `1`; the export's
`meta` record carries the same number. Without it a future reader cannot tell
which schema wrote a given file, and ANTS-3757 and ANTS-3758 both build on this
one.

```sql
CREATE TABLE project (
  project_id   INTEGER PRIMARY KEY,
  root         TEXT NOT NULL UNIQUE,   -- canonical absolute path
  name         TEXT NOT NULL,
  export_slug  TEXT NOT NULL UNIQUE,   -- filename-safe; names the export file
  legend       TEXT                    -- JSON: {status_value: project_wording}
);

-- § 7.1 of the model: "The store owns allocation", and a project may declare
-- several prefixes. MAX(id) over live items is NOT the floor — a deleted id is
-- retired, never reissued, so the high-water mark outlives the row.
CREATE TABLE id_prefix (
  project_id   INTEGER NOT NULL REFERENCES project(project_id),
  prefix       TEXT NOT NULL,          -- folded
  high_water   INTEGER NOT NULL,
  PRIMARY KEY (project_id, prefix)
);

CREATE TABLE item (
  item_pk      INTEGER PRIMARY KEY,    -- surrogate; relationship targets use it
  project_id   INTEGER NOT NULL REFERENCES project(project_id),
  id           TEXT NOT NULL,          -- verbatim, as authored
  id_fold      TEXT NOT NULL,          -- lowercased; the identity key
  id_parses    INTEGER NOT NULL CHECK (id_parses IN (0,1)),
  status       TEXT NOT NULL CHECK (status IN
                 ('planned','in-progress','shipped','considered','dropped')),
  headline     TEXT NOT NULL,
  layman       TEXT,
  kind         TEXT NOT NULL,          -- CHECK against the 21-value enum, § 7.4
  source       TEXT,
  priority     INTEGER CHECK (priority IS NULL OR priority BETWEEN 1 AND 5),
  visibility   TEXT NOT NULL DEFAULT 'public'
                 CHECK (visibility IN ('public','internal')),
  milestone    TEXT,
  resolution   TEXT,
  body         TEXT,
  section_id   INTEGER NOT NULL REFERENCES section(section_id),
  created      TEXT, last_modified TEXT, shipped TEXT,   -- ISO 8601, § 2.4
  lanes        TEXT,                   -- JSON array
  evidence     TEXT,                   -- JSON array
  extras       TEXT,                   -- JSON object: the § 4.3 tail
  provenance   TEXT NOT NULL,          -- JSON object: field -> § 7.7 value
  UNIQUE (project_id, id_fold)
);
```

**Every closed enum in the model is a `CHECK`, not a comment.** § 7.4 of the
standard says "Writes accept canonical values only", and a `TEXT NOT NULL`
column accepts anything. `kind`'s 21 values are written as a `CHECK … IN (…)`
list in the implementation; they are elided above only for width.

**`section_id` is `NOT NULL`** because the model's § 3.1 makes `section` a write
obligation and its § 3.3 gives migration a default — an item filed nowhere is
not a state the model has.

**The rest of § 3.1's write tier is enforced in the write path, not by the
schema, and that is forced rather than chosen.** `source`, `created`,
`last_modified`, and the conditional `layman` / `priority` / `resolution` /
`shipped` are all required *at write* — but the model's § 3.3 requires
migration to accept historical items that carry none of them. A `NOT NULL`
column cannot express "required of new writes, absent on migrated rows", so the
column stays nullable and `RoadmapStore::putItem()` refuses a write missing any
field its tier demands. The gate is real and it is this spec's; it simply
cannot live in DDL. Distinguishing the two cases is what `provenance` (§ 7.7 of
the model) is for: a nullable column plus `provenance: migrated` is a
historical item, while the same null on an `asserted` row is a bug.

**`sort_order` is deliberately absent, and this diverges from the standard on
its face.** § 5 of the model settles the precedence — "the element list is the
source and `sort_order` is recomputed from it" — which makes it derived; but
§ 4.1 lists `sort_order` in the `write` obligation tier, which makes it
authored. Both cannot hold. This spec follows § 5, stores the order once in
`element.position`, and computes `sort_order` at read for any caller that wants
it, because storing both is two encodings of one fact with no authority named.

**Open against the standard, not settled here:** § 4.1's obligation row should
change to `derived`, or § 5's precedence rule should go. Flagged to the author
of ANTS-3753 rather than resolved unilaterally — a spec quietly overriding its
own standard is how the two drift.

`id_fold` carries `roadmap-data-model.md` § 7.1's case-insensitive identity:
`Sh-1` and `SH-1` are one item. `id` keeps what the author wrote, because
`roadmap-format.md` § 3.5.1 makes ids append-only and rewriting one breaks
every citation.

**`extras` and `provenance` are JSON columns, not tables.** The corpus carries
a long tail of project-invented field keys — **~290 distinct keys** in total,
of which `extras` holds everything not already a § 4.1 field — almost all
appearing once or twice. A key-value table would be a sparse join per key to
reconstruct one item. `provenance` is per field by the model's § 7.7, so it is
naturally an object keyed by field name.

**SQLite ≥ 3.38 is the floor**, because JSON1 is built in by default only from
that release; earlier builds require a compile flag this project does not
control. `.github/workflows/ci.yml` and `release.yml` both run `ubuntu-22.04`,
which ships **3.37** — so either those runners move, or the queries avoid JSON1
and read the columns as opaque text. `dependencies.md` requires the floor to be
stated rather than discovered, and this is the one dependency here with a real
version constraint.

```sql
CREATE TABLE section (
  section_id  INTEGER PRIMARY KEY,
  project_id  INTEGER NOT NULL REFERENCES project(project_id),
  slug        TEXT NOT NULL,
  title       TEXT NOT NULL,
  level       INTEGER NOT NULL,
  intro       TEXT,
  parent_id   INTEGER REFERENCES section(section_id),
  UNIQUE (project_id, slug)
);

-- § 5's ordered element list. An item element points at item_pk; a narration
-- or table element carries its payload inline.
CREATE TABLE element (
  element_id  INTEGER PRIMARY KEY,
  section_id  INTEGER NOT NULL REFERENCES section(section_id),
  position    INTEGER NOT NULL,
  kind        TEXT NOT NULL CHECK (kind IN ('item','narration','table')),
  item_pk     INTEGER REFERENCES item(item_pk),
  payload     TEXT,                   -- narration prose, or JSON table
  UNIQUE (section_id, position),
  -- An 'item' element carries a reference and no payload; the other two
  -- carry a payload and no reference. Without this the schema permits
  -- kind='item' with a NULL reference, which the model has no meaning for.
  CHECK ((kind = 'item') = (item_pk IS NOT NULL)
     AND (kind = 'item') = (payload IS NULL))
);

CREATE TABLE relationship (
  rel_id      INTEGER PRIMARY KEY,
  type        TEXT NOT NULL CHECK (type IN ('splits-from','blocked-by',
                'duplicate-of','supersedes','relates-to','specified-by')),
  src_pk      INTEGER NOT NULL REFERENCES item(item_pk),
  dst_pk      INTEGER REFERENCES item(item_pk),   -- item target
  dst_path    TEXT,                                -- document target
  CHECK ((dst_pk IS NULL) != (dst_path IS NULL))
);

-- Two PARTIAL indexes, not one UNIQUE over all four columns: SQLite treats
-- NULLs as distinct in a unique index, and the CHECK above guarantees one of
-- dst_pk / dst_path is always NULL — so the combined constraint would never
-- fire and INV-6 would have no storage-level backing at all.
CREATE UNIQUE INDEX rel_item_uq ON relationship(type, src_pk, dst_pk)
  WHERE dst_pk IS NOT NULL;
CREATE UNIQUE INDEX rel_doc_uq  ON relationship(type, src_pk, dst_path)
  WHERE dst_path IS NOT NULL;

CREATE TABLE history (
  history_id  INTEGER PRIMARY KEY,
  item_pk     INTEGER NOT NULL REFERENCES item(item_pk),
  changed_at  TEXT NOT NULL,
  seq         INTEGER NOT NULL,       -- disambiguates edits within one second
  field       TEXT NOT NULL,
  old_value   TEXT,
  new_value   TEXT,
  UNIQUE (item_pk, changed_at, seq)
);

CREATE TABLE feedback_ref (
  item_pk     INTEGER NOT NULL REFERENCES item(item_pk),
  file        TEXT NOT NULL,
  PRIMARY KEY (item_pk, file)
);

CREATE TABLE citation (
  citation_id INTEGER PRIMARY KEY,
  item_pk     INTEGER REFERENCES item(item_pk),
  doc_path    TEXT,
  target_file TEXT NOT NULL,
  symbol      TEXT NOT NULL DEFAULT '',   -- '' not NULL, so UNIQUE bites
  CHECK ((item_pk IS NULL) != (doc_path IS NULL))
);
CREATE UNIQUE INDEX cite_item_uq ON citation(item_pk, target_file, symbol)
  WHERE item_pk IS NOT NULL;
CREATE UNIQUE INDEX cite_doc_uq  ON citation(doc_path, target_file, symbol)
  WHERE doc_path IS NOT NULL;
```

`relates-to` is symmetric per § 6 and is **stored once**, normalised so that
the endpoint whose `(export_slug, id_fold)` sorts first is `src_pk`.

**Normalising on `item_pk` would be wrong**, and subtly: surrogate rowids are
not stable across a rebuild, so a pair normalised 5→9 in the live store can
normalise 2→3 — the opposite direction — in the store rebuilt from the export.
The exported direction flips and INV-1 fails, which is exactly the hazard the
normalisation exists to remove. Every rule in this spec that orders or
de-duplicates rows uses stable identity, never a rowid.

`blocked` (§ 4.1 of the model) has **no column** — it is `derived`, computed at
read from `blocked-by` edges, and therefore never exported. INV-2's "every store
row" means rows, and a derived value is not one.

### 2.4 Export serialisation

One file per project, **JSON Lines**, in the private `claude-config` repo at
`roadmap-export/<export_slug>.jsonl`. JSON Lines rather than one JSON document
because a changed item is then a one-line diff, which is the review granularity
ANTS-3753 asked for.

**No surrogate key is ever serialised.** `item_pk`, `project_id`,
`section_id`, `element_id`, `rel_id` and `history_id` are SQLite rowids
assigned in insertion order. A rebuild inserts in *export* order while the
source store was built in *document* order, and any row ever deleted leaves a
gap that never recurs — so a serialised rowid guarantees INV-1 fails. Records
address each other by stable identity instead:

| Target | Addressed by |
|---|---|
| project | `export_slug` (implicit — one file per project) |
| item | `id_fold`, or `{project, id_fold}` when cross-project |
| section | `slug` |

#### Serialisation: RFC 8785, not a hand-written rule list

**Each line is one JSON object serialised per [RFC 8785](https://www.rfc-editor.org/rfc/rfc8785),
the JSON Canonicalization Scheme (JCS).** A published canonicalisation is used
rather than a table of house rules because the house-rules approach does not
converge: every freedom pinned by hand is a rule that some other passage can
contradict, and the list is never provably complete.

JCS fixes, by reference: **object key order** (lexicographic by UTF-16 code
unit), **whitespace** (none), **string escaping** (the short forms `\b \t \n
\f \r \" \\`, everything else below U+0020 as `\u00xx` lowercase hex, nothing
above U+007F escaped, `/` never escaped), **number formatting**, and **UTF-8
without a BOM**. Nothing in this spec restates those; a restatement is a second
copy that will drift.

Two consequences an implementer must not discover the hard way:

- **Key order is JCS's, not a hand-chosen reading order.** The record shapes
  below are written in a readable order for humans; the *emitted* order is
  lexicographic. Do not treat the examples as byte-exact.
- **`QJsonDocument::toJson(Compact)` is not certified JCS.** `QJsonObject` does
  hold keys sorted, which is most of the way there, but Qt's escaping and
  number formatting are not specified to match the RFC. The writer must be
  validated against the RFC's own test vectors, or use a JCS implementation —
  assuming Qt's output conforms is the obvious mistake here.

JCS canonicalises **one JSON value**. It says nothing about a file of them, so
this spec still owns everything below.

#### Record types

Every line is one JSON object with a `t` discriminator.

```jsonl
{"t":"meta","schema":1,"project":"ants-terminal","name":"Ants Terminal"}
{"t":"id_prefix","prefix":"ants","high_water":3759}
{"t":"legend","status":"in-progress","wording":"In progress (active commit work…)"}
{"t":"section","slug":"performance-2","title":"Performance","level":3,"parent":null,"intro":null}
{"t":"item","id":"ANTS-1234","id_parses":true,"section":"performance-2","status":"shipped","kind":"perf","headline":"…","layman":"…","source":"…","priority":2,"visibility":"public","milestone":null,"resolution":"…","body":"…","created":"2026-07-30","last_modified":"2026-07-30","shipped":"2026-07-30","lanes":["vt"],"evidence":[],"extras":{},"provenance":{"kind":"defaulted"}}
{"t":"element","section":"performance-2","position":0,"kind":"item","ref":"ants-1234"}
{"t":"element","section":"performance-2","position":1,"kind":"narration","payload":"Prose belonging to no item."}
{"t":"element","section":"performance-2","position":2,"kind":"table","payload":{"header":["A","B"],"rows":[["1","2"]]}}
{"t":"rel","type":"blocked-by","src":"ants-1234","dst":"ants-1200"}
{"t":"rel","type":"specified-by","src":"ants-1234","dst_path":"docs/specs/ANTS-1234-thing.md"}
{"t":"citation","src":"ants-1234","file":"src/vtparser.cpp","symbol":"VtParser::feed"}
{"t":"citation","doc":"docs/specs/ANTS-1234-thing.md","file":"src/vtparser.cpp","symbol":"VtParser::feed"}
{"t":"feedback_ref","item":"ants-1234","file":"Vestige_Ants_MCP_Feedback.md"}
{"t":"history","item":"ants-1234","at":"2026-07-30T09:15:00Z","seq":0,"field":"status","old":"planned","new":"shipped"}
```

Every variant is shown because a variant with no shape is a variant nobody can
export: `element` appears three times (one per `kind`), `rel` twice (item
target and document target), `citation` twice (item-anchored and
doc-anchored).

- `id` is emitted verbatim; **`id_fold` is derived and never exported** — the
  reader recomputes it, so the two cannot disagree.
- **`meta` carries no export timestamp.** A date inside a byte-identity
  contract defeats it: two exports of an unchanged store would differ across
  midnight, and every regeneration would churn the committed file. `root` is
  likewise absent — it is a machine-local absolute path, and the export is
  committed to a repo shared between machines.
- `history.seq` disambiguates two edits to one field within the same second;
  without it the `history` sort is not total.

#### File-level rules JCS does not cover

| Freedom | Pinned to |
|---|---|
| Record-type order | `meta`, `id_prefix`, `legend`, `section`, `item`, `element`, `rel`, `citation`, `feedback_ref`, `history` — all **ten**, in that order |
| `id_prefix` order | by `prefix`, code-unit order |
| `legend` order | by `status`, in the model's § 7.3 declared enum order |
| `section` order | by `slug`, code-unit order |
| `item` order | by the id sort below |
| `element` order | by `(section, position)` |
| `rel` order | by `(type, src, dst, dst_path)`, code-unit order |
| `citation` order | by `(src, doc, file, symbol)`, code-unit order |
| `feedback_ref` order | by `(item, file)`, code-unit order |
| `history` order | by `(item, at, seq)` — total by construction, since `seq` is unique per `(item, at)` |
| Absent vs null | a field with no value is **omitted**; `null` appears only where a record shape shows it (`parent`, `intro`, `milestone`) |
| Empty string vs absent | an empty string is a **value** and is emitted; absence is omission |
| Empty containers | `lanes`, `evidence`, `extras` are emitted as `[]` / `{}` when empty, never omitted — they always exist on an item |
| Line ending | `\n`, including after the final line |
| Numbers | integers only. JCS's number rules are defined over ECMAScript doubles; the model has no non-integer value, so that machinery never engages |
| Timestamps | dates `YYYY-MM-DD`; `history.at` is `YYYY-MM-DDTHH:MM:SSZ` — UTC, second precision, always `Z` |

#### Id sort order

Items sort by a **variable-length numeric-segment tuple**, because no simpler
rule survives the corpus:

1. Fold the id, then split it into maximal **alphabetic** and **numeric** runs. Separators (`-`, `_`) are **discarded, not compared** — they carry no ordering meaning and treating them as runs would make `3D_E-0007` and `3DE-0007` sort differently for no reason.
   - `ants-1234` → `("ants", 1234)`
   - `pass-43-5-b` → `("pass", 43, 5, "b")`
   - `3d_e-0007` → `(3, "d", "e", 7)` — note it **starts numeric**
2. Compare run by run. Two runs of the same type compare naturally: numeric by value, alphabetic by **UTF-16 code-unit order** on the folded text (never locale collation — `LC_COLLATE` varies by machine and the export must not).
3. **Where the two runs differ in type, numeric sorts before alphabetic.** Not cosmetic: `3D_E` is a live prefix, so `3d_e-0007` begins with a numeric run where `ants-1234` begins with an alphabetic one, and without this rule the sort is undefined between two real projects' ids.
4. A shorter tuple sorts before a longer one sharing its prefix (`pass-43-5` before `pass-43-5-b`).
5. **Tie-break on the raw `id`, code-unit order.** Required, not defensive: zero-padding is write-side only (`roadmap-format.md` § 3.5.1), so `CL-9` and `CL-0009` fold to the identical tuple and the sort would otherwise be non-total — which makes INV-1 fail *intermittently*, the worst way for it to fail.
6. Quarantined ids (`id_parses = 0`) sort **last**, among themselves by raw `id`.

Lexical sorting is not available (`ANTS-10` before `ANTS-9`), and neither is
zero-padded string sorting (the corpus has genuinely unpadded ids). A
two-element `(prefix, numeric)` tuple is also insufficient: it cannot express
`PASS-43-5-B`, and splitting at the last hyphen would sort `PASS-9-*` after
`PASS-10-*`.

### 2.5 Concurrency

Two writers exist: the Ants GUI process and the MCP verb layer, which may be a
second process (`CLAUDE.md` — the live binary runs from a home-drive copy, and
a Quake instance plus a regular window is a normal state).

**Pragmas, applied on every connection at open — not once at creation.**
`foreign_keys` is per-connection and defaults **off**, so without this every
`REFERENCES` in § 2.3 is decorative:

```sql
PRAGMA journal_mode = WAL;      -- persistent, but re-asserting is harmless
PRAGMA foreign_keys = ON;       -- per-connection; OFF by default
PRAGMA busy_timeout = 5000;     -- ms; matches ConfigWriteLock's deadline
```

- **WAL gives one writer and concurrent readers**, which is the access shape
  here — but it does **not** queue a second writer. Without `busy_timeout` the
  second writer gets an immediate `SQLITE_BUSY`. The timeout is set to 5000 ms
  to match `ConfigWriteLock`'s existing deadline rather than introduce a second
  timeout constant.
- **The export reads inside one deferred transaction.** It spans many
  statements, and without a transaction a commit landing mid-export tears the
  file — half pre-change, half post-change, and INV-1 fails against a store
  nobody corrupted.
- **Every write transaction opens `BEGIN IMMEDIATE`, never plain `BEGIN`.** A
  deferred transaction that reads and then writes must upgrade to a write lock,
  and SQLite returns `SQLITE_BUSY` on that upgrade **without honouring
  `busy_timeout`** — the classic WAL upgrade deadlock, and with two writers
  named above it is reachable in normal use. `BEGIN IMMEDIATE` takes the write
  lock up front, where the timeout does apply.
- **Store creation is itself a race.** Two processes finding no store both run
  the DDL. Creation happens inside `BEGIN IMMEDIATE` with `CREATE TABLE IF NOT
  EXISTS`, and `user_version` is set only by the transaction that created the
  tables.
- **The export is written temp-then-`rename(2)`, inside the lock's scope.** It
  is a whole-file rewrite of the only durable copy of a primary store; a crash
  midway through an in-place write truncates the backup, which is the one
  outcome worse than not having written it.
- **`ConfigWriteLock`** (`src/configbackup.h`) wraps the export **write**. It
  is the project's existing RAII `flock(2)` guard, and
  `tests/features/concurrent_writer_lock/` already locks its behaviour. The
  export is a whole-file rewrite, exactly the read-modify-write shape that
  guard exists for; reusing it beats a second locking scheme (`CLAUDE.md` § 3).
- **On a failed acquire the export ABORTS and reports.** The guard is advisory
  and its header leaves the choice to the caller — skip, or proceed
  unprotected. Proceeding unprotected is not available here: the model's § 9
  says a silent backup failure is worse than no backup, because it stops anyone
  checking. A failed acquire is a loud error, never a skipped write.

## 3. Invariants

- **INV-1** — Export, rebuild from that export, re-export ⇒ byte-identical files. This holds **per project and across the whole corpus** — a corpus-wide rebuild must also preserve the cross-project relationships the model's INV-4 allows, which a per-project round-trip cannot witness. *Test:* `tests/features/roadmap_store_roundtrip/` — export all projects, rebuild into a temp store, re-export, `cmp` each pair, and assert the cross-project edge count is unchanged. *Breaks when:* any § 2.4 file-level rule is left unpinned; seed the fixture with two items whose insertion order differs from their id order, and one cross-project `blocked-by`.
- **INV-2** — The export is complete: every store row, and every **non-surrogate** column of it, survives the round-trip. *Test:* `roadmap_store_roundtrip` — after rebuild, per-table `COUNT(*)` matches for all **ten** tables, and a column-wise diff matches, **joining rows on stable identity** (`(export_slug, id_fold)` for items, `slug` for sections) with `item_pk`, `project_id`, `section_id`, `element_id`, `rel_id` and `history_id` excluded. *Breaks when:* a writer drops a whole column — `provenance`, say — which round-trips byte-identically and preserves every row count, so INV-1 and a count-only check both pass on a lossy store. The surrogate exclusion is not a weakening: § 2.4 guarantees rowids differ after a rebuild, so a diff including them fails against a *correct* implementation.
- **INV-3** — Item identity is case-folded within a project: inserting `Sh-1` then `SH-1` into one project raises a uniqueness violation, and the same pair in two different projects does not. *Test:* `roadmap_store_identity`. *Breaks when:* `UNIQUE (project_id, id_fold)` is written against `id`.
- **INV-4** — An off-grammar id is stored verbatim with `id_parses = 0` and is never rewritten. *Test:* `roadmap_store_identity` inserts `[Cl9]`; assert `id` round-trips exactly and no dash is inserted. *Breaks when:* a writer normalises ids on the way in.
- **INV-5** — Export item order follows § 2.4's numeric-segment sort and is **total**. *Test:* `roadmap_store_roundtrip` seeds `ANTS-9`, `ANTS-10`, `CL-9`, `CL-0009`, `PASS-43-5`, `PASS-43-5-B`, `PASS-9-1` and one quarantined id, and asserts the exact emitted order. *Breaks when:* the writer sorts lexically (`ANTS-10` lands before `ANTS-9`); or splits at the last hyphen (`PASS-9-1` lands after `PASS-43-5`); or omits the raw-bytes tie-break, leaving `CL-9` and `CL-0009` unordered — a non-total sort makes INV-1 fail intermittently, which is the worst way for it to fail.
- **INV-6** — `relates-to` is stored once, normalised on stable identity. Writing A→B then B→A yields exactly one row, and the stored direction survives a rebuild. *Test:* `roadmap_store_schema` writes both directions and asserts one row; `roadmap_store_roundtrip` asserts the direction is unchanged after export-rebuild-export. *Breaks when:* normalisation keys on `item_pk` — rowids are reassigned by the rebuild, so the direction can flip and the re-export differs, defeating the invariant's own purpose.
- **INV-7** — The resolved store path is under `GenericDataLocation + "/ants-terminal"` and never under any cache location. *Test:* `roadmap_store_schema` asserts on the **resolved path at runtime** — it must equal `QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/ants-terminal/roadmap.sqlite"`, and **neither cache root may be a prefix of it** — the resolved path must not start with `writableLocation(CacheLocation)`, nor with `writableLocation(GenericCacheLocation) + "/ants-terminal"` (where the project's real caches live). The direction matters: `~/.cache/ants-terminal/roadmap.sqlite` is *not* a prefix of the cache root, so the reversed comparison passes for exactly the placement this invariant forbids. A source-grep for `CacheLocation` near the store is a secondary guard only. *Breaks when:* the path is composed from a constant defined elsewhere — a grep then sees one hit and the location is decided somewhere it never looked, so a grep-only test passes against a store sitting in the cache.
- **INV-8** — A project is keyed on its **canonical** root: two paths that canonicalise to the same directory are one project, and a genuinely different root is a different project. *Test:* `roadmap_store_schema` — (a) insert via a symlinked path and via the real path, assert **one** `project_id`; (b) insert two genuinely distinct roots, assert two. *Breaks when:* `root` is stored unresolved, which makes (a) yield two rows. Case (a) is the one that matters: `mcp-caches.md`'s never-shadow rule applied to rows instead of files, and asserting *two* there would certify exactly the bug it forbids.
- **INV-9** — Every export write path acquires `ConfigWriteLock`, and aborts loudly when it cannot. *Test:* `roadmap_store_concurrency` — hold the lock, attempt an export, assert it returns an error and **wrote no bytes**; release, assert it then succeeds. *Breaks when:* the writer treats `!acquired()` as permission to proceed unprotected. Phrasing matters here: `flock` is advisory, so a non-cooperating writer *can* interleave — the testable claim is about our writer, not about the file.
- **INV-10** — `provenance` is per field, in both directions: editing `headline` through the store sets `provenance.headline` to `asserted` **and** leaves `provenance.kind` untouched. *Test:* `roadmap_store_schema` asserts both halves. *Breaks when:* provenance is stored per item — or when the writer never updates provenance at all, which a one-sided "kind is unchanged" assertion would happily certify.
- **INV-11** — Every closed enum is rejected at the storage layer, not merely documented: `status`, `kind` (the 21-value set), `visibility`, `element.kind`, `relationship.type` and each `provenance` value outside its set, or a `priority` outside 1–5, all fail on insert. *Test:* `roadmap_store_schema` attempts one invalid insert per enum and asserts each is refused. *Breaks when:* the enums are written as SQL comments, which is how the first draft of § 2.3 had them.
- **INV-12** — The export writer streams: peak RSS during an export of the whole corpus rises by **less than 4 MiB** above the pre-export baseline, against an export several times that size. *Test:* `roadmap_store_roundtrip` samples RSS immediately before and at peak during the export call and asserts the **delta**. *Breaks when:* the writer builds one `QString` and writes it at the end — which passes every other invariant here. The measurement must be a delta: a Qt process's absolute RSS exceeds the export's byte size before any work is done, so an absolute ceiling is unachievable and would be quietly relaxed until it passed.
- **INV-13** — Every cross-record reference in the export resolves to a declared stable key, and no surrogate value is emitted under any name. *Test:* `roadmap_store_roundtrip` parses the export and asserts that every `ref` / `src` / `dst` / `item` / `section` value resolves to an `id` or `slug` declared elsewhere in the same file. *Breaks when:* a writer serialises straight from `SELECT *` — the natural implementation, which silently breaks INV-1 on any store that has ever deleted a row. A name-based grep for `item_pk` and friends is **not** sufficient: it passes against a writer emitting the same rowids under a different key (`"section":3`), and it false-positives on free-text `body` or `extras` content.
- **INV-14** — `history` is capped: no item retains more than 50 revisions, oldest evicted first. *Test:* `roadmap_store_schema` writes 60 revisions to one item's field and asserts 50 remain, the oldest gone. *Breaks when:* the cap is documented in § 4 and never implemented — which is the normal fate of a budget with no invariant behind it.

## 4. RAM / build cost

**Memory.** The corpus is 4.91 MiB of markdown across ten roadmaps
(`find . -maxdepth 2 -iname 'roadmap.md' -printf '%s\n' | awk '{s+=$1} END {print s}'`
→ 5,147,623 bytes, run from the projects' parent directory — **case-insensitively**,
since one project's file is lowercase and a `[Rr]oadmap.md` glob silently
returns a tenth of the true figure). Qt holds text as UTF-16, so that is
**~10 MiB of `QString`** before container and `QVariant` overhead — call it
**12–14 MiB** fully materialised. That is affordable once and unaffordable
repeatedly, so:

- **No query materialises the whole corpus by default.** Every read verb takes
  a project filter or a `LIMIT`, defaulting to the caller's own project.
- **The export writer streams** — one item per line, never a whole-corpus
  string in memory. This is a correctness requirement as well as a budget one:
  a 5 MiB `QString` built by concatenation reallocates repeatedly.
- **No in-process row cache in this spec.** The 100 ms parse cache
  `roadmap_query` uses today (ANTS-1117) is a markdown-parsing cache with
  nothing to cache once reads are SQL. Adding one later needs its own eviction
  policy and its own id.

**`history` is capped at 50 revisions per item**, oldest evicted first. It is
the one table with no natural bound and it is exported, so it grows the backup
too; `specs.md` § 4 requires a named cap rather than a promise to measure later.
50 is chosen to cover an item's realistic edit life while bounding the export at
roughly the item count times a small constant. Migration backfills at most one
history row per item (ANTS-3757), so the cap binds only on live editing.

**Disk.** The store is roughly the markdown's size plus indexes and the JSON
columns — order **10–15 MiB** for this corpus. The export is comparable. Both
are small enough that no eviction policy is needed beyond `history`'s.

**Indexes.** SQLite does **not** auto-index foreign keys, and every join this
schema implies would otherwise scan: `item(project_id)`, `item(section_id)`,
`element(section_id)`, `relationship(src_pk)`, `relationship(dst_pk)`,
`history(item_pk)`, `citation(item_pk)`, `feedback_ref(item_pk)`. All are
declared with the schema.

**Build.** One new library target, `roadmapstore`, linking `Qt6::Core` and
`Qt6::Sql`. One new feature-test bundle. A new **external** runtime dependency
(§ 2.1) across five packaging carriers and CI.

## 5. Out of scope

- **Migration** — reading the ten existing markdown roadmaps into this schema, including pass-headings status normalisation and bulk id allocation. Tracked by **ANTS-3757**.
- **The published render, the backup repo's push cadence, and the fate of `roadmap_query` / `roadmap_log` / `RoadmapDialog`.** Tracked by **ANTS-3758**.
- **The health-check suite** (duplicate ids, a feedback file citing an id no project owns, a spec `Status` disagreeing with its bullet). Scheduling and per-check behaviour are **ANTS-3758**; this spec provides only the schema they query. Two further checks belong there explicitly, so their absence here is a decision rather than an oversight: the **acyclicity** of `splits-from` / `blocked-by` / `duplicate-of` / `supersedes` (the model's § 6 makes it a whole-store property, which no per-row constraint can express), and the model's INV-1 **second leg** — comparing the *committed* export against a fresh export of the live store. INV-1 here implements only the re-export-equality leg, which the model itself calls insufficient alone.
- **Multi-machine sync.** The export lands in a git repo and git is the sync mechanism; the store itself is single-machine. This is a **permanent exclusion**, not deferred work — a store synchronised at the SQLite level is a different product, and the export exists precisely so it is not needed.
- **Full-text search over bodies.** A **permanent exclusion** for this spec: the store's job is to hold the model and round-trip it, and search is a read surface, which ANTS-3758 owns. Not deferred, so no id is owed here — if search is wanted, it is that spec's decision whether FTS5 or a `LIKE` scan serves it.

## 6. Tests

Feature tests under `tests/features/`, label `features;fast`:

| Directory | Covers |
|---|---|
| `roadmap_store_schema/` | INV-6, INV-7, INV-8, INV-10, INV-11 |
| `roadmap_store_identity/` | INV-3, INV-4 |
| `roadmap_store_roundtrip/` | INV-1, INV-2, INV-5, INV-12, INV-13 |
| `roadmap_store_concurrency/` | INV-9 |

All thirteen invariants are covered; none is a grep-only check.

Per the project convention (`CLAUDE.md`, `testing.md`), each test must be
verified to **fail against pre-implementation source** before the
implementation is restored. For INV-1 that means a deliberately unpinned
writer — emit object keys in insertion order and confirm the round-trip
comparison fails — because a round-trip test written against an already-correct
writer passes for reasons it never checked.

## 7. Cross-doc impact

- **`CMakeLists.txt`** — `Sql` added to the Qt6 `COMPONENTS` list; new `roadmapstore` target.
- **Five packaging carriers, not one.** Each pins Qt modules independently, and a missing SQL driver fails at **runtime**, not at link — so a green build proves nothing here:
  - `packaging/opensuse/ants-terminal.spec` — `BuildRequires: cmake(Qt6Sql)` plus a runtime `Requires` for the driver package.
  - `packaging/debian/control` — the QSQLITE plugin in `Depends`.
  - `packaging/archlinux/PKGBUILD` — the Qt SQL module in `depends`.
  - `packaging/flatpak/za.co.antsprojectshub.AntsTerminal.yml` — the module in the manifest.
  - AppImage — bundles `sqldrivers/libqsqlite.so` **and** `libsqlite3.so.0` (§ 2.1: the plugin links system sqlite; the plugin alone is not enough).
- **`.github/workflows/ci.yml`** — three jobs install an explicit apt list, including the `qt62-baseline` job. Without the SQL module every feature test below fails at runtime in CI. Both this and `release.yml` run `ubuntu-22.04`, whose SQLite is below § 2.3's floor.
- **`.github/workflows/release.yml`** — carries its **own** apt list for the AppImage build, separate from `ci.yml`'s.
- **`README.md`** — "the only thing it needs to run is Qt6" becomes false by the same argument as `CLAUDE.md`, and the per-distro build-dependency lines each gain a package.
- **[`mcp-caches.md`](../standards/mcp-caches.md)** — gains a row recording that `roadmap.sqlite` is **not** a cache, is not path-keyed, and must never be added to a GC sweep. Its never-shadow invariant applies to the `project.root` column instead (INV-8).
- **[`roadmap-data-model.md`](../standards/roadmap-data-model.md)** — its § 9 currently says the schema, the export record types and concurrency are the spec's; once this ships, those bullets point here.
- **`CLAUDE.md`** — the "Qt6 is the only runtime dep" line becomes **false** and must be amended (§ 2.1): the QSQLITE driver links system `libsqlite3`.
- **[`docs/subsystems.md`](../subsystems.md)** — gains the `roadmapstore` lane. The module map moved there in ANTS-1292; `CLAUDE.md` carries only a pointer, and `indie_review_partition` derives one review lane per entry from that file, so a lane added to `CLAUDE.md` instead would never be reviewed.
- **`CHANGELOG.md`** — new store, user-invisible until ANTS-3758 lands the render.

## 8. Open questions

- **How does the writer locate the `claude-config` checkout?** § 2.4 gives a path *inside* that repo but never how the repo itself is resolved — an unspecified input to the export. Candidates: a config key, a fixed `~/.claude` sibling, or a `.ants/project.json` field (ANTS-2160). Needs deciding before implementation, and it may belong to ANTS-3758 with the rest of the backup cadence.
- **Which project's export carries a cross-project relationship?** The model's INV-4 allows them, and the export is per project, so a single-project rebuild cannot resolve the far side. Three options — the source project holds it, both hold it, or a corpus-level file does — and each changes what a per-project INV-1 round-trip means.
- **Does `id_fold` derive in the schema or the writer?** § 2.3 stores it as a column with nothing tying it to `id`. A `GENERATED ALWAYS AS (lower(id))` column would make INV-3 unfalsifiable by a non-folding writer; a plain column leaves the constraint satisfiable while the writer misbehaves.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 1 | 2026-07-30 | 2 (design + contract, grounding + cross-doc) | 7 / 11 / 10 / 10 / 0 | All 38 verified, 0 dismissed, all draft defects. Both lanes independently falsified § 2.1's engine justification — Qt's QSQLITE driver links **system** `libsqlite3` and ships in a separate package, so a third-party runtime dep does enter the graph and `CLAUDE.md`'s "Qt6 is the only runtime dep" needs amending, not defending. § 2.4 was a list of constraints on a format rather than a format: no record shape, no discriminator, surrogate rowids serialised (which makes INV-1 unachievable on any store that has ever deleted a row), and 4 of 8 record types unordered. Rewritten with nine record types, a worked example line, every freedom pinned, and a variable-length numeric sort that survives `PASS-43-5-B`. Four invariants would have certified a broken store — INV-8 asserted the exact failure its *Breaks when* forbade, INV-7 named a `QStandardPaths` constant that resolves to the wrong directory on this project, INV-1+2 were jointly satisfied by a column-lossy writer, INV-10 by a writer ignoring provenance. INV-11..13 added. Enum CHECKs, FK pragma, `busy_timeout`, export transaction, `history` cap, indexes, and five packaging carriers where the draft named one. **Doc grew 321 → 511 lines while being fixed** — noted per Phase 2 as a size signal, not re-partitioned mid-run. |
| 2 | 2026-07-30 | 2 (same partition, cold) | 9 / 12 / 12 / 10 / 0 | Findings rose against loop 1 and **~75% were collateral from loop 1's own fixes** — so the loop was stopped and the cause addressed rather than dispatching loop 3. Root cause: § 2.4 guaranteed byte-identity via a hand-written table of ~20 serialisation freedoms, and every freedom pinned created new places for the document to contradict itself. The clincher was external: `QJsonObject` sorts keys, so the pinned field order was unproducible with Qt's own JSON classes. **Resolved by decision (user, 2026-07-30): adopt RFC 8785 (JCS)** — key order, whitespace, escaping, numbers and encoding now come from a published standard, and this spec keeps only the file-level rules JCS cannot cover. Also fixed: loop 1's edit had inserted prose *inside* a `sql` fence (17 fence lines, odd) so the schema rendered as one code block — `doc_integrity` and `spec_lint` both pass an unbalanced-fence document, which is why it shipped; a fence-parity check is now part of the post-fix pass. `id_prefix` gained an export record (its high-water mark was otherwise lost on every rebuild, reissuing live ids); INV-7's cache-prefix test was inverted and passed for a store in the cache; `relates-to` normalised on a rowid in contradiction of its own section; the relationship and citation UNIQUEs never fired because SQLite treats NULLs as distinct; `BEGIN IMMEDIATE`, temp+rename, and a SQLite 3.38 floor added. INV-14 added. |
