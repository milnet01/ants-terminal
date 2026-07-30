# ANTS-3756 — Roadmap store: engine, location and schema

**Status:** spec draft (2026-07-30).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3756 (ANTS-3753 split, spec seam 1 of 3).
**Blocker for:** ANTS-3761 (export format), ANTS-3757 (migration), ANTS-3758 (publish + consumer cutover).
**Pairs with:** [`roadmap-data-model.md`](../standards/roadmap-data-model.md) — that standard defines *what an item is*; this spec defines how it is stored.

**Layman:** The database that holds every project's to-do items — where it lives, what its tables look like, and how two copies of the app share it safely.

## 1. Problem

[`roadmap-data-model.md`](../standards/roadmap-data-model.md) defines three
artifacts — store, export, published render — and § 9 of that standard
deliberately stops before the schema. Nothing yet says what a table looks like,
where the store lives, or how two Ants processes sharing one avoid corrupting
it. (The export's serialisation was originally here too; it is now
[ANTS-3761](ANTS-3761-roadmap-export-format.md) — see § 2.4.)

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

**Mode 0600, on the store and both SQLite sidecars.** The store holds every
project's full technical body, including `visibility: internal` items — which
the model's § 7.5 says exist precisely to hold "security findings that are
still open". Every other state writer in this project applies owner-only
permissions via `setOwnerOnlyPerms()` (`src/secureio.h`), and this file is the
most sensitive of them. WAL mode creates `roadmap.sqlite-wal` and
`roadmap.sqlite-shm` alongside it, and **both carry the same content**, so
securing only the main file would be theatre.

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
  root         TEXT UNIQUE,            -- canonical absolute path; store-local,
                                       -- NULL until first local open (§ 2.4)
  name         TEXT NOT NULL,
  -- Names the export file, so its charset is a filesystem trust boundary, not
  -- a nicety: ANTS-3761 interpolates it into a path, and a slug containing
  -- '/' or '..' escapes the export directory. Derived from `name` by
  -- ASCII-lowercasing, replacing every run of non-[a-z0-9] with '-', and
  -- trimming leading/trailing '-'; collisions are resolved by appending -2, -3.
  export_slug  TEXT NOT NULL UNIQUE
                 CHECK (export_slug GLOB '[a-z0-9]*'
                    AND export_slug NOT GLOB '*[^a-z0-9._-]*'
                    AND export_slug NOT GLOB '*..*'),
  legend       TEXT NOT NULL DEFAULT '{}'  -- {status_value: project_wording}
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
  -- THREE origins, not a boolean. A synthesised PASS-N-M[-S] id does not match
  -- roadmap-format § 3.5.1's grammar (it need not end in digits), yet the
  -- model's § 7.1 says it "**is** an ID for every purpose". A boolean conflates
  -- it with the genuinely off-grammar ids that must be quarantined.
  id_origin    TEXT NOT NULL CHECK (id_origin IN
                 ('parsed','synthesised','quarantined')),
  status       TEXT NOT NULL CHECK (status IN
                 ('planned','in-progress','shipped','considered','dropped')),
  headline     TEXT NOT NULL,
  layman       TEXT,
  kind         TEXT NOT NULL,          -- CHECK vs the model's § 7.4 21-value enum
  source       TEXT NOT NULL,          -- model § 3.3 defaults it, so never absent
  priority     INTEGER CHECK (priority IS NULL OR priority BETWEEN 1 AND 5),
  visibility   TEXT NOT NULL DEFAULT 'public'
                 CHECK (visibility IN ('public','internal')),
  milestone    TEXT,
  resolution   TEXT,
  body         TEXT,
  section_id   INTEGER NOT NULL REFERENCES section(section_id),
  -- Format pinned HERE, not in the export: if the store held any other ISO
  -- 8601 form the export would normalise it, the rebuild would store the
  -- normalised text, and INV-2's column diff would fail on a correct writer.
  created      TEXT CHECK (created       IS NULL OR created       GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'),
  last_modified TEXT CHECK (last_modified IS NULL OR last_modified GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'),
  shipped      TEXT CHECK (shipped       IS NULL OR shipped       GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'),
  -- NOT NULL with a default, because ANTS-3761 § 2.4 emits these as [] / {}
  -- when empty and never omits them. Nullable columns would make NULL and
  -- empty export identically, and the rebuild's column diff would then fail
  -- against a correct writer.
  lanes        TEXT NOT NULL DEFAULT '[]',
  evidence     TEXT NOT NULL DEFAULT '[]',
  extras       TEXT NOT NULL DEFAULT '{}',
  provenance   TEXT NOT NULL,          -- JSON object: field -> § 7.7 value
  UNIQUE (project_id, id_fold)
);
```

**Every closed enum in the model is a `CHECK`, not a comment.** § 7.4 of the
standard says "Writes accept canonical values only", and a `TEXT NOT NULL`
column accepts anything. `kind`'s 21 values are written as a `CHECK … IN (…)`
list in the implementation; they are elided above only for width.

**`section_id` is `NOT NULL`, and the default that makes it satisfiable is this
spec's own.** The model's § 3.1 makes `section` a write obligation, but its
§ 3.3 defaults only `kind` and `source` — `section` appears in neither the
defaulted list nor the left-empty one. Migration will meet items that appear
before any heading, so this spec supplies what the standard does not: **each
project gets a synthetic root section** (`slug` `""`, level 0, no title) and
un-sectioned items are filed there, marked `provenance.section = defaulted`.
An item filed nowhere is not a state the model has; an item filed in a section
nobody wrote is honest and recoverable.

**The rest of § 3.1's write tier is enforced in the write path, not by the
schema, and that is forced rather than chosen.** `created`, `last_modified`,
and the conditional `layman` / `priority` / `resolution` / `shipped` are
required *at write* — but the model's § 3.3 requires migration to accept
historical items carrying none of them. (`source` is **not** in this set: the
model's § 3.3 gives it a migration default of `planned`, so it is never absent
and is `NOT NULL` above, next to `kind` for the same reason.) A `NOT NULL`
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

**JSON columns are STORED in the same RFC 8785 canonical form § 2.4 emits.**
Not a tidiness rule — INV-2 diffs columns, and a rebuild writes canonical text
while the original store holds whatever its writer produced, so an
uncanonicalised store fails that diff against a *correct* implementation. This
covers `item.lanes`, `item.evidence`, `item.extras`, `item.provenance`,
`project.legend` and `element.payload`. Canonicalising once at the write path
makes the export a copy rather than a transformation.

**The store does not depend on JSON1, and that is a decision rather than a
preference.** `extras` and `provenance` are read and written as opaque text;
nothing in this spec requires `json_extract` or `json_each`. A report that
wants to query inside them may use JSON1 where available, but no invariant here
does.

The reason is that the obvious alternative is foreclosed. JSON1 became a
default-on build only in **SQLite 3.38**, and two runners predate it —
`release.yml` (`ubuntu-22.04`) and `ci.yml`'s `qt62-baseline` job. Neither can
move: `release.yml`'s pin is a deliberate **glibc-2.35** choice, because
`ubuntu-24.04` would lock the AppImage to glibc 2.39 and drop still-supported
LTS distros; and `dependencies.md` § 6 states the `qt62-baseline` pin "is *not*
a stale pin — it mirrors the § 4 Qt 6.2 floor. Do **not** bump it in the runner
sweep." Requiring 3.38 would therefore mean shipping an AppImage whose own
bundled `libsqlite3` is below the floor.

**Open, and deliberately not asserted:** Debian and Ubuntu have historically
built `libsqlite3` with `SQLITE_ENABLE_JSON1` well before 3.38, so 22.04's
3.37.2 may carry JSON1 regardless. That was not verifiable from the development
host. It does not change the decision — depending on a distro build flag is
worse than not depending on the feature — but it means the 3.38 figure is an
*upstream-default* boundary, not a measured floor for these runners. `dependencies.md` requires the floor to be
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
  -- THREE target forms, not two. The model's INV-4 allows cross-project
  -- relationships, and ANTS-3761 carries them on the source project's file —
  -- so a rebuild that cannot see the far project must still be able to STORE
  -- the edge. An unresolved edge has no dst_pk and is not a document target.
  dst_pk       INTEGER REFERENCES item(item_pk),  -- resolved item, same store
  dst_project  TEXT,                              -- far project's export_slug
  dst_id_fold  TEXT,                              -- far item's folded id
  dst_path     TEXT,                              -- document target
  CHECK (
    (dst_pk IS NOT NULL) + (dst_project IS NOT NULL) + (dst_path IS NOT NULL) = 1
  ),
  CHECK ((dst_project IS NULL) = (dst_id_fold IS NULL))
);

-- Three PARTIAL indexes, not one UNIQUE over every column: SQLite treats NULLs
-- as distinct in a unique index, and the CHECK guarantees all but one target
-- column is NULL — so a combined constraint would never fire and INV-6 would
-- have no storage-level backing at all.
CREATE UNIQUE INDEX rel_item_uq  ON relationship(type, src_pk, dst_pk)
  WHERE dst_pk IS NOT NULL;
CREATE UNIQUE INDEX rel_xproj_uq ON relationship(type, src_pk, dst_project, dst_id_fold)
  WHERE dst_project IS NOT NULL;
CREATE UNIQUE INDEX rel_doc_uq   ON relationship(type, src_pk, dst_path)
  WHERE dst_path IS NOT NULL;

CREATE TABLE history (
  history_id  INTEGER PRIMARY KEY,
  item_pk     INTEGER NOT NULL REFERENCES item(item_pk),
  changed_at  TEXT NOT NULL            -- YYYY-MM-DDTHH:MM:SSZ, UTC, always Z
                CHECK (changed_at GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z'),
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
  -- project_id is NOT redundant with item_pk: a doc-anchored citation has no
  -- item, and the export is per project, so without this column the writer
  -- cannot decide which file a doc-anchored row belongs in.
  project_id  INTEGER NOT NULL REFERENCES project(project_id),
  item_pk     INTEGER REFERENCES item(item_pk),
  doc_path    TEXT,
  target_file TEXT NOT NULL,
  symbol      TEXT NOT NULL DEFAULT '',   -- '' not NULL, so UNIQUE bites
  CHECK ((item_pk IS NULL) != (doc_path IS NULL))
);
CREATE UNIQUE INDEX cite_item_uq ON citation(item_pk, target_file, symbol)
  WHERE item_pk IS NOT NULL;
-- project_id leads: two projects each citing their own README.md are two
-- rows, not a collision.
CREATE UNIQUE INDEX cite_doc_uq  ON citation(project_id, doc_path, target_file, symbol)
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

### 2.4 Export serialisation — moved

The export's serialisation, record types, file-level ordering, id sort and
write path now live in **[ANTS-3761](ANTS-3761-roadmap-export-format.md)**.

Split out at this spec's cold-eyes cap: it is a separate contract with a
separate failure mode, and it was where nearly all of three loops' fix
collateral landed. What remains here is the store — engine, location, schema
and connection pragmas.

Two things this spec still owes the export, and they are stated here because
the schema is what guarantees them:

- **`export_slug` is `UNIQUE`** (§ 2.3), so two projects cannot overwrite one
  another's export file.
- **JSON columns are stored in RFC 8785 canonical form** (§ 2.3), so the export
  is a copy of those bytes rather than a transformation of them.


### 2.5 Concurrency

Two writers exist: the Ants GUI process and the MCP verb layer, which may be a
second process. `CLAUDE.md` records that the live binary runs from a
home-drive copy; `src/main.cpp`'s `--quake` / `--dropdown` options are what
make a dropdown instance alongside a regular window a normal state.

**Pragmas, applied on every connection at open — not once at creation.**
`foreign_keys` is per-connection and defaults **off**, so without this every
`REFERENCES` in § 2.3 is decorative:

```sql
PRAGMA journal_mode = WAL;      -- persistent, but re-asserting is harmless
PRAGMA foreign_keys = ON;       -- per-connection; OFF by default
PRAGMA synchronous  = FULL;     -- primary store, not a cache: survive power loss
PRAGMA busy_timeout = 5000;     -- ms; matches ConfigWriteLock's deadline
```

- **WAL gives one writer and concurrent readers**, which is the access shape
  here — but it does **not** queue a second writer. Without `busy_timeout` the
  second writer gets an immediate `SQLITE_BUSY`. 5000 ms matches
  `ConfigWriteLock`'s existing deadline rather than introducing a second
  timeout constant.
- **`synchronous = FULL`, not WAL's `NORMAL` default.** `NORMAL` is the right
  trade for a cache that can be rebuilt; § 1 makes this store primary, and its
  only rebuild path is the export. Durability is the whole reason it does not
  live under a cache path.
- **When `busy_timeout` expires on a write, the write fails and reports.** It
  is never retried silently and never dropped: a lost roadmap write is
  invisible to the user, and the model's § 9 argues the same way about silent
  backup failure.
- **Every write transaction opens `BEGIN IMMEDIATE`, never plain `BEGIN`.** A
  deferred transaction that reads and then writes must upgrade to a write lock,
  and SQLite returns `SQLITE_BUSY` on that upgrade **without honouring
  `busy_timeout`** — the classic WAL upgrade deadlock, reachable in normal use
  with the two writers named above. `BEGIN IMMEDIATE` takes the write lock up
  front, where the timeout does apply.
- **Store creation is itself a race.** Two processes finding no store both run
  the DDL. Creation happens inside `BEGIN IMMEDIATE` with `CREATE TABLE IF NOT
  EXISTS`, and the winner is decided by **reading `PRAGMA user_version` inside
  that same transaction**: the process that sees `0` creates the tables and
  sets it. `CREATE TABLE IF NOT EXISTS` succeeds for both and reports nothing,
  so it cannot be the discriminator.

The **export's** locking, atomicity and abort-on-failed-acquire rules moved
with it to [ANTS-3761](ANTS-3761-roadmap-export-format.md) § 2.6.


## 3. Invariants

- **INV-1** — *moved to ANTS-3761* (export round-trip byte-identity) — see [ANTS-3761](ANTS-3761-roadmap-export-format.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-2** — *moved to ANTS-3761* (export completeness) — see [ANTS-3761](ANTS-3761-roadmap-export-format.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-3** — Item identity is case-folded within a project: inserting `Sh-1` then `SH-1` into one project raises a uniqueness violation, and the same pair in two different projects does not. *Test:* `roadmap_store_identity`. *Breaks when:* `UNIQUE (project_id, id_fold)` is written against `id`.
- **INV-4** — An off-grammar id is stored verbatim with `id_origin = 'quarantined'` and is never rewritten. *Test:* `roadmap_store_identity` inserts `[Cl9]`; assert `id` round-trips exactly and no dash is inserted. *Breaks when:* a writer normalises ids on the way in.
- **INV-5** — *moved to ANTS-3761* (export item order) — see [ANTS-3761](ANTS-3761-roadmap-export-format.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-6** — `relates-to` is stored once, normalised on stable identity. Writing A→B then B→A yields exactly one row, and the stored direction survives a rebuild. *Test:* `roadmap_store_schema` writes the **higher**-sorting endpoint first and asserts the surviving row's `src` is the *lower* one — writing the lower first would pass against a writer that merely rejects the second edge without normalising anything; `roadmap_store_roundtrip` then asserts the direction is unchanged after export-rebuild-export. *Breaks when:* normalisation keys on `item_pk` — rowids are reassigned by the rebuild, so the direction can flip and the re-export differs, defeating the invariant's own purpose.
- **INV-7** — The resolved store path is under `GenericDataLocation + "/ants-terminal"` and never under any cache location. *Test:* `roadmap_store_schema` asserts on the **resolved path at runtime** — it must equal `QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/ants-terminal/roadmap.sqlite"`, and **neither cache root may be a prefix of it** — the resolved path must not start with `writableLocation(CacheLocation)`, nor with `writableLocation(GenericCacheLocation) + "/ants-terminal"` (where the project's real caches live). The direction matters: `~/.cache/ants-terminal/roadmap.sqlite` is *not* a prefix of the cache root, so the reversed comparison passes for exactly the placement this invariant forbids. A source-grep for `CacheLocation` near the store is a secondary guard only. *Breaks when:* the path is composed from a constant defined elsewhere — a grep then sees one hit and the location is decided somewhere it never looked, so a grep-only test passes against a store sitting in the cache.
- **INV-8** — A project is keyed on its **canonical** root: two paths that canonicalise to the same directory are one project, and a genuinely different root is a different project. *Test:* `roadmap_store_schema` — (a) insert via a symlinked path and via the real path, assert **one** `project_id`; (b) insert two genuinely distinct roots, assert two. *Breaks when:* `root` is stored unresolved, which makes (a) yield two rows. Case (a) is the one that matters: `mcp-caches.md`'s never-shadow rule applied to rows instead of files, and asserting *two* there would certify exactly the bug it forbids.
- **INV-9** — *moved to ANTS-3761* (the export write lock) — see [ANTS-3761](ANTS-3761-roadmap-export-format.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-10** — `provenance` is per field, in both directions: editing `headline` through the store sets `provenance.headline` to `asserted` **and** leaves `provenance.kind` untouched. *Test:* `roadmap_store_schema` asserts both halves. *Breaks when:* provenance is stored per item — or when the writer never updates provenance at all, which a one-sided "kind is unchanged" assertion would happily certify.
- **INV-11** — Every closed enum held in its **own column** is rejected at the storage layer, not merely documented: `status`, `kind` (the 21-value set), `visibility`, `element.kind`, `relationship.type`, and a `priority` outside 1–5, all fail on insert. *Test:* `roadmap_store_schema` attempts one invalid insert per enum and asserts each is refused. *Breaks when:* the enums are written as SQL comments, which is how the first draft of § 2.3 had them. **`provenance`'s values are deliberately excluded**: it is a JSON object with arbitrary keys, and validating each value needs `json_each`, which a SQLite `CHECK` may not contain — so that enum is enforced in the write path alongside § 3.1's tier, not in DDL.
- **INV-12** — *moved to ANTS-3761* (export streaming) — see [ANTS-3761](ANTS-3761-roadmap-export-format.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-13** — *moved to ANTS-3761* (no surrogate in the export) — see [ANTS-3761](ANTS-3761-roadmap-export-format.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-15** — Two processes opening a store that does not yet exist produce exactly one schema: one creates the tables and sets `user_version = 1`, the other observes `user_version = 1` inside its own `BEGIN IMMEDIATE` and creates nothing. *Test:* `roadmap_store_concurrency` forks two openers against one fresh path and asserts one `user_version` bump and one set of tables. *Breaks when:* creation gates on `CREATE TABLE IF NOT EXISTS` succeeding, which succeeds for both and reports nothing — the whole reason § 2.5 reads `user_version` instead.
- **INV-16** — A write that cannot take the lock within `busy_timeout` **fails and reports**; it is never retried silently and never dropped. *Test:* `roadmap_store_concurrency` holds a write transaction open past the timeout in one connection, attempts a write on another, and asserts an error is returned and **no row was written**. *Breaks when:* the writer swallows `SQLITE_BUSY` and returns success — a lost roadmap write is invisible to the user, which is the failure this exists to prevent.
- **INV-17** — The store and both SQLite sidecars are mode 0600. *Test:* `roadmap_store_schema` opens a store, forces a WAL checkpoint so `-wal` and `-shm` exist, and asserts `0600` on all three. *Breaks when:* only the main file is secured — the sidecars carry the same content, including `visibility: internal` items.
- **INV-18** — *moved to ANTS-3761* (export golden-file conformance) — see [ANTS-3761](ANTS-3761-roadmap-export-format.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-19** — *moved to ANTS-3761* (RFC 8785 test-vector conformance) — see [ANTS-3761](ANTS-3761-roadmap-export-format.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-14** — `history` is bounded: no item's revision count grows without limit. *Test:* `roadmap_store_schema` writes 60 revisions to one item's field and asserts the retained count is ≤ 50. *Breaks when:* the cap is documented in § 4 and never implemented — the normal fate of a budget with no invariant behind it. **The invariant deliberately asserts the bound and not *which* revisions survive**, because § 4 leaves the eviction rule open against the model's § 6; tighten this clause once that is decided, rather than encoding a rule the standard may reject.

## 4. RAM / build cost

**Memory.** § 1 measures the corpus at 4.91 MiB of markdown and names the
command; that figure is not restated here. Qt holds text as UTF-16, so a fully
materialised corpus is **~10 MiB of `QString`** before container and `QVariant`
overhead — call it **12–14 MiB**. Affordable once, unaffordable repeatedly, so:

- **No query materialises the whole corpus by default.** Every read takes a
  project filter or a `LIMIT`, defaulting to the caller's own project. This is
  a constraint **ANTS-3758 inherits** for the read verbs it owns, not a
  surface this spec defines.
- **No in-process row cache in this spec.** The 100 ms parse cache
  `roadmap_query` uses today (ANTS-1117) is a markdown-parsing cache with
  nothing to cache once reads are SQL. Adding one later needs its own eviction
  policy and its own id.

**Disk.** SQLite stores UTF-8, so the item text is ~4.9 MiB; with indexes, the
JSON columns and `history` at its cap the store is order **6–9 MiB** for this
corpus. (An earlier draft said 10–15 MiB, which had borrowed the UTF-16 RAM
figure — SQLite does not store UTF-16.)

**`history` is capped at 50 revisions per item — but the eviction rule is an
open conflict with the standard, surfaced rather than settled here.**
`specs.md` § 4 requires a named cap for unbounded growth, and `history` is the
only table without a natural bound. But the model's § 6 makes history exported
*precisely* because "the store is untracked and the render is lossy, so that
history would otherwise have nowhere to live — which makes exporting it the
point, not an optimisation." **Evicting the oldest revisions destroys the only
surviving copy of exactly what that sentence protects**, which is a worse
outcome than an oversized export.

So the cap is stated and its eviction rule is not, pending a decision by the
standard's author (as with `sort_order` above). Three candidates, in the order
this spec would rank them: cap by **total store size** rather than per item,
evicting nothing until a real bound is hit; evict from the **middle**, keeping
the creation and the most recent revisions, which are the two a reader actually
wants; or accept unbounded growth and revisit when a measured rate exists.
Migration backfills at most one history row per item (ANTS-3757), so nothing
binds until live editing accumulates — there is time to decide, and no reason
to guess now.

**Indexes.** SQLite does **not** auto-index foreign keys, and every join this
schema implies would otherwise scan: `item(section_id)`, `section(parent_id)`,
`element(section_id)`, `element(item_pk)`, `relationship(src_pk)`,
`relationship(dst_pk)`, `history(item_pk)`, `citation(item_pk)`. All are
declared with the schema. `item(project_id)` and `feedback_ref(item_pk)` are
**not** declared, because each is already the leading column of an existing
`UNIQUE (project_id, id_fold)` / composite primary key — a second index there
costs writes and buys nothing.

**Build.** One new library target, `roadmapstore`, linking `Qt6::Core` and
`Qt6::Sql`. Three new feature-test bundles. A new **external** runtime dependency
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
| `roadmap_store_schema/` | INV-6, INV-7, INV-8, INV-10, INV-11, INV-14, INV-17 |
| `roadmap_store_identity/` | INV-3, INV-4 |
| `roadmap_store_concurrency/` | INV-15, INV-16 |

All **eleven** invariants this spec retains are covered; none is a grep-only
check. The six tombstoned above are covered by
[ANTS-3761](ANTS-3761-roadmap-export-format.md) § 6.

Per the project convention (`CLAUDE.md`, `testing.md`), each test must be
verified to **fail against pre-implementation source** before the
implementation is restored. For INV-11 that means enums written as SQL
comments — confirm the invalid inserts *succeed* — because a constraint test
written against an already-constrained schema passes for reasons it never
checked.

## 7. Cross-doc impact

- **`CMakeLists.txt`** — `Sql` added to the Qt6 `COMPONENTS` list; new `roadmapstore` target.
- **Five packaging carriers, not one.** Each pins Qt modules independently, and a missing SQL driver fails at **runtime**, not at link — so a green build proves nothing here:
  - `packaging/opensuse/ants-terminal.spec` — `BuildRequires: cmake(Qt6Sql)` plus a runtime `Requires` for the driver package.
  - `packaging/debian/control` — the QSQLITE plugin in `Depends`.
  - `packaging/archlinux/PKGBUILD` — the Qt SQL module in `depends`.
  - `packaging/flatpak/za.co.antsprojectshub.AntsTerminal.yml` — the module in the manifest.
  - AppImage — bundles `sqldrivers/libqsqlite.so` **and** `libsqlite3.so.0` (§ 2.1: the plugin links system sqlite; the plugin alone is not enough).
- **`.github/workflows/ci.yml`** — three jobs install an explicit apt list, and they need different things. The two that run `ctest` need the **runtime** driver or every feature test below fails on a green build; `qt62-baseline` is a compile guard ("Build everything — no ctest") and needs only the **dev** package.
- **[`docs/standards/dependencies.md`](../standards/dependencies.md)** — § 4's minimum-supported-floors table gains SQLite, with its mandatory *Guard* column, and § 6's version map gains the new runtime dependency. That standard is what § 2.3's runner reasoning rests on, so a floor added here and not there is a floor nothing sweeps.
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
| 3 | 2026-07-30 | 2 (same partition, cold) | 8 / 12 / 12 / 12 / 1 | **Converged by cap.** Collateral outnumbered draft defects for the second loop running — the skill's stop trigger — and the doc has grown 321 → 683 lines while being fixed, so the run exits here rather than looping a fourth time. Both lanes independently verified the RFC 8785 delegation is characterised correctly, which is the loop-2 decision holding. Fixed this loop: the surrogate-key rule was four names short in two places and is now stated as a rule rather than a list; `id_fold` was declared never-exported while every reference record emitted it; `root` was `NOT NULL` and unexportable, blocking the rebuild leg outright; INV-11 asserted a CHECK over a JSON object, which SQLite cannot express; the coverage table said "thirteen" of fourteen invariants and omitted INV-14; INV-5's seeds tested neither of the sort rules loop 2 added; `source` was treated as defaultless when the model defaults it. **The SQLite floor fork was closed against evidence**: both branches of "move the runners" are foreclosed — `release.yml`'s `ubuntu-22.04` is a deliberate glibc-2.35 pin, and `dependencies.md` § 6 forbids bumping `qt62-baseline` — so the store now depends on no JSON1. Two conflicts with the standard are **surfaced, not resolved**: `sort_order`'s obligation tier, and `history` eviction, which as drafted would destroy the only copy of what the model's § 6 says the export exists to preserve. Deferred tail filed as **ANTS-3760**. |
| 3-split | 2026-07-30 | none — no reviewer dispatched | — | **Provenance row, not a review.** Acting on loop 3's split recommendation (user-approved): the export half — serialisation, record types, file-level ordering, id sort and the export write path — moved to **[ANTS-3761](ANTS-3761-roadmap-export-format.md)**, taking INV-1, 2, 5, 9, 12 and 13 with it. Those numbers are **tombstoned in place here, never reflowed** (`specs.md` § 5.5), so this spec's sequence has gaps by design. What stayed is the database: engine and its dependency cost, location, schema, connection pragmas. Also folded in from ANTS-3760's tail while editing: `synchronous = FULL` and a stated busy-timeout failure mode (a primary store owes durability a cache does not), the `user_version == 0` creation-race discriminator, the duplicated corpus measurement deleted from § 4, and the disk figure corrected — it had borrowed the UTF-16 RAM number, and SQLite stores UTF-8. **This spec's three loops do not transfer to ANTS-3761**, which runs the gate from loop 1 on its own bytes; and this half is itself now materially smaller than what those loops reviewed, so its next gate run is against a changed document. |
| 4 | 2026-07-30 | 1 (store lane, cold, counterpart also read) | 5 / 6 / 6 / 6 / 0 | First gate on the post-split document. Two **seam failures** where ANTS-3761 emitted fields this schema had no column for: cross-project `rel` (no `dst_project`/`dst_id_fold`, and the CHECK forbade an unresolved edge outright) and per-project `citation` (no `project_id`, so a doc-anchored row had no path to a project and `cite_doc_uq` collided across projects). Both fixed here. `section_id NOT NULL` was justified by a migration default the model's § 3.3 does **not** contain — it defaults only `kind` and `source` — so this spec now supplies the synthetic root section itself. `id_parses` was a boolean conflating genuinely off-grammar ids with synthesised `PASS-N-M[-S]` ones, which the model's § 7.1 calls real ids; replaced by a three-value `id_origin`. § 4 carried **two** `**Disk.**` paragraphs, the second restating verbatim a figure the first retracts. Added: 0600 on the store and both WAL sidecars (INV-17), an `export_slug` charset CHECK (it is interpolated into a path), timestamp CHECKs so the export cannot normalise what the store holds loosely, `NOT NULL DEFAULT` on the JSON columns, and INV-15/16 for the creation race and busy-timeout policy — § 2.5 previously had no invariant and no test at all. |
