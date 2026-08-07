# ANTS-3756 — Roadmap store: engine, location and schema

**Status:** implemented (2026-07-31) — `src/roadmapstore.{h,cpp}` in
`ants_roadmapstore_lib`, behind `tests/features/roadmap_store_schema/`,
`roadmap_store_identity/` and `roadmap_store_concurrency/`. Two known gaps are
tracked rather than closed: ANTS-3760 (the deferred tail this spec left at its
cold-eyes cap) and
[ANTS-3781](ANTS-3781-roadmap-store-schema-upgrade.md) (the schema-upgrade path,
now specified and built — it completes this document's § 2.3 version contract).
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
  Qt SQL build with no driver fails at **run time, not link time** — precisely,
  `QSqlDatabase::addDatabase("QSQLITE")` returns an *invalid* database rather
  than throwing, so the observable failure is at `isValid()` / `open()` and a
  caller that checks neither gets silence. This is a packaging step no green
  build will catch.

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

**Why the MCP helper sources stay `Qt6::Core`-only.** Sixteen **sources inside
`ants_core_lib`** carry an explicit `Qt6::Core-only` comment
(`grep -c "Qt6::Core-only" CMakeLists.txt` → 16; `read_region.cpp`,
`codebase_index.cpp`, `docs_index.cpp`, …) so the test bundles link without GUI
or extra modules. They are **source files annotated inside one library**, not
sixteen libraries — the file declares eight `add_library` targets in total, so
the earlier reading was impossible on its face. The store does **not** join
`ants_core_lib`: it is its own `roadmapstore` library, and the pure helpers keep
their current link surface.

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

`PRAGMA user_version` carries the schema version, starting at `1` — the shape of
**these tables**. The export's `meta` record carries its own, independent
version of the **record** shape
([ANTS-3781](ANTS-3781-roadmap-store-schema-upgrade.md) § 2.3 held the two
apart; they started equal and move separately now, so a table-shape bump no
longer invalidates every export ever written). Without a version a future reader
cannot tell which record shape wrote a given file, and ANTS-3757 and ANTS-3758
both build on this one.

**A version this binary does not know is refused, and the two directions differ.**
A store whose `user_version` is **higher** than the binary's is opened
**not at all** — not read-only, refused with a message naming both numbers.
Read-only sounds like the safe option and is not: a newer schema can move
meaning rather than only add to it, so a confident partial read is worse than no
read. A **lower** `user_version` is an upgrade, and
**[ANTS-3781](ANTS-3781-roadmap-store-schema-upgrade.md)** owns it — this
document first assigned it to ANTS-3757, which shipped without building one, so
the sentence is corrected rather than left pointing at a closed spec. It is now
built: `createSchema()` routes every non-zero version below this build's to
`applyUpgrades()`.

**Two milestones are involved and an earlier draft of this paragraph conflated
them** (ANTS-3781 § 1 separates them). A store becomes **reachable** — real
stores in real hands — at ANTS-3758's cutover, which is the deadline recorded
here, and one version-1 store already exists on this machine outside any test's
temp directory. A store *below* the running binary's version becomes
**possible** only once `kSchemaVersion` moves, which is ANTS-3815. ANTS-3782
§ 2.1 and ANTS-3796 § 2.1 each add a column *within* version 1, and **the ground
they actually recorded is the first one** — "no store is reachable from
user-facing code yet", as § 2.3's own `source_path` DDL comment and § 7's
ANTS-3796 bullet both state it. The second ground held as well, and is the more durable of the
two: with `kSchemaVersion` never having moved, no below-version store was
possible for a rung to meet. Both are named here rather than reassigning theirs,
because an earlier revision of this paragraph credited them to the second ground
alone and contradicted its own § 4 and § 7. Reachability is what makes the gap
matter; the bump is what makes it occur.
This matters here because the launcher can leave an older binary on disk
(`CLAUDE.md`) and this store is primary — the one file with no source to rebuild
from except its own export.

```sql
CREATE TABLE project (
  project_id   INTEGER PRIMARY KEY,
  -- Canonical absolute path, per QFileInfo::canonicalFilePath() and nothing
  -- else (INV-8). Store-local and never exported (ANTS-3761 § 2.3), so it is
  -- NULL on a project this machine has not opened. A path that fails to
  -- canonicalise is REFUSED, never stored: Qt returns '' for a non-existent
  -- path, and '' under UNIQUE would fuse every such project into one.
  root         TEXT UNIQUE,
  name         TEXT NOT NULL,
  -- Names the export file, so its charset is a filesystem trust boundary, not
  -- a nicety: ANTS-3761 interpolates it into a path, and a slug containing
  -- '/' or '..' escapes the export directory. Derived from `name` by
  -- ASCII-lowercasing, replacing every run of non-[a-z0-9] with '-', and
  -- trimming leading/trailing '-'; collisions are resolved by appending -2, -3.
  -- A name with no [a-z0-9] at all derives to '', which the CHECK refuses --
  -- the writer substitutes 'project' and takes the collision suffix, so the
  -- rule stays total rather than depending on this corpus's naming habits.
  -- The charset is EXACTLY what the derivation can emit. An earlier draft also
  -- admitted '.' and '_', which no derivation could produce, so the schema
  -- accepted slugs no writer could make and needed a separate '..' guard to
  -- stay safe; forbidding '.' outright makes that guard unnecessary.
  export_slug  TEXT NOT NULL UNIQUE
                 CHECK (export_slug GLOB '[a-z0-9]*'
                    AND export_slug NOT GLOB '*[^a-z0-9-]*'),
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
  -- GENERATED, not a plain column the writer fills: SQLite refuses an INSERT
  -- that targets it, so a writer CANNOT store an unfolded identity key. See
  -- the note below the table.
  id_fold      TEXT GENERATED ALWAYS AS (lower(id)) VIRTUAL,
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
  -- NO section column. An item's filing is its `element` row -- see below.
  -- Format pinned HERE, not in the export: if the store held any other ISO
  -- 8601 form the export would normalise it, the rebuild would store the
  -- normalised text, and INV-2's column diff would fail on a correct writer.
  -- The GLOB checks SHAPE ONLY -- '2026-13-45' passes. That is deliberate:
  -- calendar validity needs arithmetic a CHECK cannot express, and it is the
  -- write path's job alongside the other write-path rules below. What the
  -- GLOB buys is that no OTHER ISO 8601 spelling can enter the column.
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
  provenance   TEXT NOT NULL DEFAULT '{}',  -- JSON object: field -> § 7.7 value
  UNIQUE (project_id, id_fold)
);
```

**Every closed enum in the model is a `CHECK`, not a comment.** § 7.4 of the
standard says "Writes accept canonical values only", and a `TEXT NOT NULL`
column accepts anything. `kind`'s 21 values are written as a `CHECK … IN (…)`
list in the implementation; they are elided above only for width.

**`item` has no `section` column, and filing lives in exactly one place: the
item's `element` row.** The model's § 5 makes a section's ordered element list
the serialisation of its contents, so the element row already says both *which*
section an item is in and *where* in it. A `section_id` column beside it would
be a second encoding of the same fact with no authority named for which wins —
which is precisely the argument § 5 of the model settles against `sort_order`,
and it would be incoherent to accept it there and reproduce it here. **Decided
by the standard's author, 2026-07-30**, on the same grounds as that decision.

The consequence is that "every item is filed" stops being a `NOT NULL` column
and becomes **INV-20**: exactly one `element` row of `kind = 'item'` references
each item. That is the same guarantee enforced one layer up, and it is strictly
stronger — `NOT NULL` never forbade an item with *two* element rows, or with an
element row in a different section from its own `section_id`.

Migration still needs somewhere to put items that appear before any heading. The
model's § 3.1 makes `section` a write obligation while its § 3.3 defaults only
`kind` and `source`, so `section` appears in neither the defaulted list nor the
left-empty one, and this spec supplies what the standard does not: **each
project gets a synthetic root section** (`slug` `""`, level 0), and un-sectioned
items get an element row there, marked `provenance.section = defaulted`. Its
`title` is the **empty string**, not NULL — `section.title` is `TEXT NOT NULL`,
so "no title" has to be a value, and `''` is the one a renderer can skip without
a special case. An item filed nowhere is not a state the model has; an item
filed in a section nobody wrote is honest and recoverable.

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

**`sort_order` has no column, and the standard now agrees.** Order is stored
once, in `element.position`, and `sort_order` is computed at read for any caller
that wants an integer rank. Storing both would be two encodings of one fact with
no authority named for which wins when they drift.

This was a live conflict when this spec was drafted: the model's § 5 made
`sort_order` derived from the element list while its § 4.1 listed it in the
`write` obligation tier. **Settled by the standard's author, 2026-07-30 —
§ 4.1's row is now `derived`** and § 3.1's write tier no longer names it, so
this spec and its standard say the same thing. Recorded here rather than merely
fixed, because the schema's shape is the reason the question got asked.

`id_fold` carries `roadmap-data-model.md` § 7.1's case-insensitive identity:
`Sh-1` and `SH-1` are one item. `id` keeps what the author wrote, because
`roadmap-format.md` § 3.5.1 makes ids append-only and rewriting one breaks
every citation.

**`id_fold` is a `GENERATED` column, and that is a correctness decision rather
than a convenience.** As a plain column it is a second copy of `id`, and INV-3's
uniqueness constraint stays satisfiable while the writer quietly fails to fold —
the constraint fires on whatever the writer chose to put there. Generated from
`lower(id)`, the failure is unreachable. Verified on SQLite 3.53.2 rather than
assumed, because two properties had to hold together and neither is obvious:

```
sqlite> INSERT INTO item (id, …) VALUES ('Sh-1', …);   -- id_fold => 'sh-1'
sqlite> INSERT INTO item (id, …) VALUES ('SH-1', …);
Error: UNIQUE constraint failed: item.project_id, item.id_fold
sqlite> INSERT INTO item (id, id_fold, …) VALUES ('Zz-1', 'WRONG', …);
Parse error: cannot INSERT into generated column "id_fold"
```

`VIRTUAL`, not `STORED`: `UNIQUE (project_id, id_fold)` builds an index that
materialises the value anyway, so `STORED` would write it to the row as well and
buy nothing.

`lower()` is SQLite's ASCII-only fold, which is the one
[ANTS-3761](ANTS-3761-roadmap-export-format.md) § 2.3 pins for the export's
reference form — the two must agree or a reference resolves to nothing, and
`QString::toLower()`'s full Unicode mapping is the wrong one on both sides.

**This sets a real SQLite floor: 3.31** (2020-01-22), where generated columns
landed. Both constrained runners clear it — `ubuntu-22.04` carries 3.37.2 — so
unlike the JSON1 question below, this dependency is satisfiable today. It is the
figure `dependencies.md` § 4 **will record** once this ships — that table has no
SQLite row yet, and § 7 owns adding it.

**`extras` and `provenance` are JSON columns, not tables.** The corpus carries
a long tail of project-invented field keys — **~290 distinct keys counted across
the whole corpus**, of which `extras` holds everything not already a § 4.1 field
— almost all appearing once or twice. (`roadmap-data-model.md` § 4.3's "over 280
distinct keys" counts the *tail* — the same survey, one figure minus the handful
of keys that are already model fields. The two are not in conflict, but they are
easy to read as one number stated twice.) A key-value table would be a sparse join per key to
reconstruct one item. `provenance` is per field by the model's § 7.7, so it is
naturally an object keyed by field name.

**`provenance` is `derived` in the model yet it has a column, and the other two
`derived` fields do not** — which reads as an inconsistency and is not. The
model's § 3 defines `derived` as "the store computes it and an author may never
write it", and that is a rule about *who writes*, not about *where it lives*.
`blocked` recomputes from `blocked-by` edges and `sort_order` from the element
list, so storing either would be a second copy — the § 5 argument. `provenance`
recomputes from **nothing**: it records how each field got its value, which is
knowable only at the moment of writing. Dropping it loses the distinction
between a migrated null and an asserted one, which is the distinction the whole
§ 3.1 write-tier gate rests on. So it is stored, and it is exported.

**JSON columns are STORED in the same RFC 8785 canonical form the export emits**
([ANTS-3761](ANTS-3761-roadmap-export-format.md) § 2.2).
Not a tidiness rule — INV-2 diffs columns, and a rebuild writes canonical text
while the original store holds whatever its writer produced, so an
uncanonicalised store fails that diff against a *correct* implementation. This
covers `item.lanes`, `item.evidence`, `item.extras`, `item.provenance`,
`project.legend`, and `element.payload` **only when `kind = 'table'`**.
Canonicalising once at the write path makes the export a copy rather than a
transformation.

**`element.payload` is polymorphic and only one of its forms is JSON** —
narration prose when `kind = 'narration'`, a JSON table when `kind = 'table'`.
Canonicalising prose as JSON is undefined rather than merely wasteful, so
narration payload is stored as the author's text and the export emits it as a
JSON *string*, which JCS then canonicalises like any other string.

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
  -- Added by ANTS-3782 § 2.1. Which source file this section was read from,
  -- project-root-relative; NULL is the live roadmap. It is the only record of
  -- that fact -- the migration plan holds a source index and is discarded at
  -- commit -- so without it ANTS-3758 re-emits a rotated archive back into
  -- ROADMAP.md. In this DDL rather than an ALTER, and at user_version 1: no
  -- store was reachable from user-facing code when this landed, so there was
  -- nothing to migrate and a bump would have manufactured an upgrade case
  -- nothing implemented. Both halves of that argument have since expired --
  -- ANTS-3781 built applyUpgrades(), so a later column is an ALTER in a rung
  -- AS WELL AS an edit here -- ANTS-3781 § 2.1 requires both, as two
  -- expressions of one change -- and ANTS-3815 makes the first bump. See § 2.3.
  source_path TEXT,
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
  -- SQLite cannot defer a UNIQUE constraint, so reordering a section (the
  -- commonest curating write, per the model's § 5) cannot renumber in place.
  -- Rewrite the whole section's positions inside one BEGIN IMMEDIATE, offset
  -- into a scratch range first (position += 1000000), then back down.
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
  CHECK ((dst_project IS NULL) = (dst_id_fold IS NULL)),
  -- No item relates to itself under any type. Distinct from the whole-store
  -- acyclicity check ANTS-3758 owns: that one needs a graph walk, this is a
  -- single-row property and so belongs in DDL, where it costs nothing.
  CHECK (dst_pk IS NULL OR dst_pk <> src_pk)
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

-- INV-20's "at most one" half. A partial UNIQUE over one column expresses
-- "no item is filed twice" exactly; only the "at least one" half needs the
-- write path. This does NOT make the element(item_pk) index redundant --
-- this one's WHERE is on `kind`, so a bare `item_pk = ?` lookup cannot use it.
CREATE UNIQUE INDEX elem_item_uq ON element(item_pk) WHERE kind = 'item';
```

`relates-to` is symmetric per § 6 and is **stored once**, normalised so that
the endpoint whose `(export_slug, id_fold)` sorts first is `src_pk` — **but only
when both endpoints resolve to rows in this store.**

The qualifier is not caution, it is the difference between a satisfiable rule
and an unsatisfiable one. `src_pk` is `NOT NULL REFERENCES item(item_pk)`, so an
unresolved cross-project endpoint **cannot** be `src_pk` however it sorts, and
the model's INV-4 makes that edge legal. So: an edge with an unresolved endpoint
is stored with the **local** item as `src_pk`, whichever way the pair would
otherwise have sorted, and it is **not** re-normalised if the far project later
arrives — a rebuild that can suddenly see one more project must never silently
flip a stored direction, which is the hazard normalisation exists to remove.

**Normalising on `item_pk` would be wrong**, and subtly: surrogate rowids are
not stable across a rebuild, so a pair normalised 5→9 in the live store can
normalise 2→3 — the opposite direction — in the store rebuilt from the export.
The exported direction flips and INV-1 fails, which is exactly the hazard the
normalisation exists to remove. Every rule in this spec that orders or
de-duplicates rows uses stable identity, never a rowid.

`blocked` (§ 4.1 of the model) has **no column** — it is `derived`, computed at
read from `blocked-by` edges, and therefore never exported. INV-2's "every store
row" means rows, and a derived value is not one.

**Four cross-row constraints are enforced in the write path, and SQLite is the
reason.** Three are same-project rules — the model's § 4.1 says "Items are never
global", so an `element` in project A must not reference project B's item, a
`section` must not parent a section in another project, and a `citation`'s
`project_id` must match its item's. The fourth is **INV-20**: exactly one
`element` row of `kind = 'item'` per item. Nothing above forbids any of them:
each compares columns across *two rows*, and a SQLite `CHECK` may not contain a
subquery — verified, not assumed (`CREATE TABLE … CHECK (a IN (SELECT …))`
fails with "subqueries prohibited in CHECK constraints"). So `RoadmapStore`
validates all four before insert, alongside § 3.1's obligation tier — the same
place, for the same reason, and stated here so their absence from the DDL reads
as a limit of the engine rather than an oversight. **`relationship` is
deliberately not in this list**: the model's INV-4 allows cross-project edges,
which is what `dst_project` exists for.

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

Two writers exist, and they are **two Ants processes** — not a process and a
service. The MCP verb layer is *not* a second process: `src/remotecontrol.cpp`
notes that verbs "all run on the GUI thread (`dispatch()` is called from the
`QLocalServer` readyRead handler, which the GUI thread owns)", so a verb writing
to the store shares its instance's connection. The genuine second writer is a
second instance, which `src/main.cpp`'s `--quake` / `--dropdown` options make a
normal state: a dropdown instance alongside a regular window is the shipped
configuration, not an edge case.

**Pragmas, applied on every connection at open — not once at creation.**
`foreign_keys` is per-connection and defaults **off**, so without this every
`REFERENCES` in § 2.3 is decorative:

```sql
PRAGMA busy_timeout = 5000;     -- ms; matches ConfigWriteLock's deadline. FIRST:
                                -- nothing that can block runs before it is set.
                                -- 30000 on the Bulk profile — see below.
PRAGMA foreign_keys = ON;       -- per-connection; OFF by default
PRAGMA synchronous  = FULL;     -- primary store, not a cache: survive power loss
PRAGMA journal_size_limit = 67108864;  -- 64 MiB; bounds the WAL, not the store
PRAGMA journal_mode = WAL;      -- persistent, but re-asserting is harmless
```

**Which of those persist is a question to answer by measuring, not by
reading.** `journal_mode` survives the connection — it lives in the database
header. **`journal_size_limit` does not**, despite reading exactly like a file
setting: set it, reconnect, read it back, and SQLite answers `-1`. That is
recorded here because the plausible classification is wrong, and wrong in a
direction that fails silently — a sibling project on this machine (RetroDB)
classified it as file-level and moved it to a once-per-boot init path, which
left it not in force on any of the connections that actually serve requests.
The tests assert it per connection for that reason.

**Two access profiles, differing in exactly two settings.** `Access::Interactive`
is the default; `Access::Bulk` is for a writer that *knows* it may queue behind
a long transaction — migration (ANTS-3757) and the export.

| | Interactive | Bulk |
|---|---|---|
| `busy_timeout` | 5000 ms | **30000 ms** |
| `cache_size` | SQLite's 2 MiB default | **16 MiB** |

30 s is RetroDB's figure, reached there after "database is locked" under
concurrent bulk jobs. **INV-16 is unchanged by it** — both profiles still fail
and report at their deadline, neither retries silently. A single 30 s deadline
everywhere was rejected: an interactive roadmap edit that hangs for half a
minute before erroring reads as a freeze rather than an error.

**Two standard performance pragmas are deliberately NOT set, and the reason is
a budget rather than a doubt.** `mmap_size` would map the store into the
address space, making the export's own reads **resident** and breaking
[ANTS-3761](ANTS-3761-roadmap-export-format.md) INV-12's "peak RSS delta under
4 MiB" for reasons unrelated to whether the writer streams. `temp_store =
MEMORY` would build a sort's temp b-tree in RAM, and `history` is bounded at
250 MiB — spilling to disk is the safer failure. Both are ordinary wins
elsewhere; here they trade against a stated memory budget and lose. The tests
assert their *defaults*, so a later performance sweep cannot switch them on
without meeting that budget first.

**`PRAGMA optimize` runs on close.** It ANALYZEs only what the connection
touched, and it is the difference between a query plan chosen from real row
counts and one chosen from none.

- **WAL gives one writer and concurrent readers**, which is the access shape
  here — but it does **not** queue a second writer. Without `busy_timeout` the
  second writer gets an immediate `SQLITE_BUSY`. 5000 ms matches
  `ConfigWriteLock`'s existing deadline rather than introducing a second
  timeout constant.
- **Entering WAL is the one statement `busy_timeout` does not cover, and the
  store supplies the retry itself.** `PRAGMA journal_mode = WAL` takes an
  EXCLUSIVE lock **below** the busy handler, so a second instance opening the
  same fresh store gets an immediate `SQLITE_BUSY` on that statement however
  long the deadline is. Found by implementing, not by reading: INV-15's forked
  openers failed the open on **18 of 25 runs** with `busy_timeout` already at
  5000. `RoadmapStore::enableWal()` retries until the pragma reports `wal`,
  bounded by **the same** 5000 ms — a retry loop, not a second constant. A
  successful exec reporting a mode other than `wal` counts as not-yet-done:
  SQLite answers with the mode still in force rather than with an error.
  Ordinary opens exit on the first pass, WAL being persistent.
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
- **Opening an existing store must not take the write lock.** Reading
  `user_version` needs only a shared lock, which WAL grants alongside an active
  writer, so `createSchema()` checks it *outside* a transaction first and
  returns when the schema is already current. Without that check every
  ordinary open queues behind any active writer — measured at the full 5000 ms
  deadline, and it is what made the concurrency suite take 5118 ms instead of
  115 ms. The check is an optimisation and **not** the discriminator: the
  authoritative read is still the one inside `BEGIN IMMEDIATE` below, so the
  creation race is decided exactly as it was.
- **Store creation is itself a race.** Two processes finding no store both run
  the DDL. Creation happens inside `BEGIN IMMEDIATE`, and the winner is decided
  by **reading `PRAGMA user_version` inside that same transaction**: the process
  that sees `0` creates the tables and sets it. `CREATE TABLE IF NOT EXISTS`
  succeeds for both and reports nothing, so it cannot be the discriminator —
  and because it cannot, the DDL is written **without** `IF NOT EXISTS`. That
  is the stricter of the two: `user_version` already guarantees the loser never
  reaches the DDL, so a `CREATE TABLE` that *does* run against an existing table
  means the discriminator has regressed, and it should fail loudly rather than
  succeed quietly. Amended at implementation (row 8-impl) — the drafted `IF NOT
  EXISTS` would have masked exactly the failure INV-15 exists to catch.
- **Which opener created the schema must be observable at the moment it is
  decided, not inferred afterwards.** Winner and loser end identical — one set
  of tables at `user_version = 1` — so `RoadmapStore::createdSchema()` reports
  it per connection. Without it INV-15 has no assertion that a
  `CREATE TABLE IF NOT EXISTS` design would fail.

The **export's** locking, atomicity and abort-on-failed-acquire rules moved
with it to [ANTS-3761](ANTS-3761-roadmap-export-format.md) § 2.6.


## 3. Invariants

- **INV-1** — *moved to ANTS-3761* (export round-trip byte-identity) — see [ANTS-3761](ANTS-3761-roadmap-export-format.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-2** — *moved to ANTS-3761* (export completeness) — see [ANTS-3761](ANTS-3761-roadmap-export-format.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-3** — Item identity is case-folded **within** a project: inserting `Sh-1` then `SH-1` into one project raises a uniqueness violation, and the same pair in two *different* projects does not. *Test:* `roadmap_store_identity` asserts both legs, plus a third — that an INSERT naming `id_fold` explicitly is **refused**, which is what makes the folding structural rather than a habit of the current writer. *Breaks when:* the unique constraint is written against `id`, so `Sh-1` and `SH-1` become two items; or the project scope is dropped, so two projects legitimately holding the same id collide. The second leg is not padding: `roadmap-data-model.md` § 7.1 says the same id may exist in two projects, and a store keyed on `id_fold` alone rejects a corpus the model requires.
- **INV-4** — An off-grammar id is stored verbatim with `id_origin = 'quarantined'` and is never rewritten. *Test:* `roadmap_store_identity` inserts `[Cl9]`; assert `id` round-trips exactly and no dash is inserted. *Breaks when:* a writer normalises ids on the way in.
- **INV-5** — *moved to ANTS-3761* (export item order) — see [ANTS-3761](ANTS-3761-roadmap-export-format.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-6** — `relates-to` is stored once, normalised on stable identity. Writing A→B then B→A yields exactly one row, and the stored direction survives a rebuild. *Test:* `roadmap_store_schema` writes the **higher**-sorting endpoint first and asserts the surviving row's `src` is the *lower* one — writing the lower first would pass against a writer that merely rejects the second edge without normalising anything. A second leg asserts the **unresolved** case (§ 2.3): an edge whose far endpoint is cross-project keeps the local item as `src`, whichever way the pair sorts. The survives-a-rebuild leg belongs to [ANTS-3761](ANTS-3761-roadmap-export-format.md)'s `roadmap_export_roundtrip`, which is where an export-rebuild-export cycle exists — this spec owns no round-trip bundle, and naming one here would invent a fourth directory that § 6 does not budget. *Breaks when:* normalisation keys on `item_pk` — rowids are reassigned by the rebuild, so the direction can flip and the re-export differs, defeating the invariant's own purpose; or the rule is applied unconditionally, which is unsatisfiable for an unresolved endpoint because `src_pk` is `NOT NULL`.
- **INV-7** — The resolved store path is under `GenericDataLocation + "/ants-terminal"` and never under any cache location. *Test:* `roadmap_store_schema` asserts on the **resolved path at runtime** — it must equal `QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/ants-terminal/roadmap.sqlite"`, and **neither cache root may be a prefix of it** — the resolved path must not start with `writableLocation(CacheLocation)`, nor with `writableLocation(GenericCacheLocation) + "/ants-terminal"` (where the project's real caches live). The direction matters: `~/.cache/ants-terminal/roadmap.sqlite` is *not* a prefix of the cache root, so the reversed comparison passes for exactly the placement this invariant forbids. A source-grep for `CacheLocation` near the store is a secondary guard only. *Breaks when:* the store is placed under a cache root — which the runtime assertion catches wherever the path was composed, and which a **grep-only** test does not: composing the path from a constant defined elsewhere leaves the grep one hit to look at while the location is decided somewhere it never looked. The grep is named as secondary for that reason; the invariant is the runtime comparison.
- **INV-8** — A project is keyed on its **canonical** root, where *canonical* means `QFileInfo::canonicalFilePath()` and nothing else (§ 2.3). Two paths that canonicalise to the same directory are one project; a genuinely different root is a different project; and a path that **cannot** be canonicalised is refused rather than stored. *Test:* `roadmap_store_schema` — (a) insert via a symlinked path and via the real path, assert **one** `project_id`; (b) insert two genuinely distinct roots, assert two; (c) register two *different* non-existent roots and assert **both are refused and neither wrote a row**. *Breaks when:* `root` is stored unresolved, which makes (a) yield two rows — the case that matters, since `mcp-caches.md`'s never-shadow rule applied to rows instead of files means asserting *two* there would certify exactly the bug it forbids. It breaks the other way when the writer stores `canonicalFilePath()`'s return value unchecked: Qt returns an **empty string** for a path that does not exist, so two unrelated missing roots both write `''`, and `root TEXT UNIQUE` then fuses them into one project — a shadow created by the very call meant to prevent one. Leg (c) is what catches it; (a) and (b) both pass against that writer.
- **INV-9** — *moved to ANTS-3761* (the export write lock) — see [ANTS-3761](ANTS-3761-roadmap-export-format.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-10** — `provenance` is per field, in both directions: editing `headline` through the store sets `provenance.headline` to `asserted` **and** leaves `provenance.kind` untouched. *Test:* `roadmap_store_schema` asserts both halves. *Breaks when:* provenance is stored per item — or when the writer never updates provenance at all, which a one-sided "kind is unchanged" assertion would happily certify.
- **INV-11** — Every closed enum held in its **own column** is rejected at the storage layer, not merely documented: `status`, `kind` (the 21-value set), `id_origin`, `visibility`, `element.kind`, `relationship.type`, and a `priority` outside 1–5, all fail on insert. `id_origin` is on that list and was once missing from it, which is the shape of the failure this invariant exists to catch: a closed enum in its own column with a `CHECK` in § 2.3 and no test naming it passes a per-enum sweep that never looks. *Test:* `roadmap_store_schema` attempts one invalid insert per enum and asserts each is refused. *Breaks when:* the enums are written as SQL comments, which is how the first draft of § 2.3 had them. **`provenance`'s values are deliberately excluded**: it is a JSON object with arbitrary keys, and validating each value needs `json_each`, which a SQLite `CHECK` may not contain — so that enum is enforced in the write path alongside § 3.1's tier, not in DDL.
- **INV-12** — *moved to ANTS-3761* (export streaming) — see [ANTS-3761](ANTS-3761-roadmap-export-format.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-13** — *moved to ANTS-3761* (no surrogate in the export) — see [ANTS-3761](ANTS-3761-roadmap-export-format.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-15** — Two processes opening a store that does not yet exist produce exactly one schema: one creates the tables and sets `user_version = 1`, the other observes `user_version = 1` inside its own `BEGIN IMMEDIATE` and creates nothing. *Test:* `roadmap_store_concurrency` forks two openers against one fresh path, released together through a pipe, and asserts **exactly one reports `createdSchema()`** while the other reports opening a store already at `user_version = 1` — after the fact the two are indistinguishable, so the discriminator has to be observed as it is made. Neither may fail: the loser waits out the write lock. The raced store's table set is then compared against one a **single** process creates in a fresh directory — derived rather than hardcoded, so the assertion survives the schema growing. *Breaks when:* creation gates on `CREATE TABLE IF NOT EXISTS` succeeding, which succeeds for both and reports nothing — the whole reason § 2.5 reads `user_version` instead.
- **INV-16** — A write that cannot take the lock within `busy_timeout` **fails and reports**; it is never retried silently and never dropped. *Test:* `roadmap_store_concurrency`, in two legs, because the deadline and the policy fail independently. (a) The **effective** deadline on a fresh store connection is 5000 ms. (b) With one connection holding a write transaction open, a `putItem()` on another fails, returns a non-empty error, and leaves **no row**; the blocked connection's deadline is shortened to 100 ms so the suite does not wait five seconds to re-assert a constant leg (a) already owns. Leg (a) asserts the *effective* deadline deliberately: Qt's QSQLITE plugin sets 5000 ms of its own accord (`QSQLITE_BUSY_TIMEOUT`), measured by dropping the pragma and reading it back, so "our pragma ran" is not observable — drifting the constant is what reddens it. The pragma stays regardless: a durability contract should not rest on an undocumented driver default a Qt upgrade can change. *Breaks when:* the writer swallows `SQLITE_BUSY` and returns success — a lost roadmap write is invisible to the user, which is the failure this exists to prevent.
- **INV-17** — The store and both SQLite sidecars are mode 0600. *Test:* `roadmap_store_schema` opens a store, **performs a write transaction, and — with that connection still open** — asserts `0600` on all three. Both halves of that recipe are load-bearing, and an earlier draft got each of them wrong: it is the **write** that creates `-wal` and `-shm`, not a checkpoint, and SQLite **deletes both when the last connection closes**. Verified on SQLite 3.53.2 — after `PRAGMA journal_mode=WAL` alone the sidecars are absent, after a committed write both exist, a `wal_checkpoint(TRUNCATE)` leaves them exactly as they were, and closing the connection removes them. So a test that checkpoints, or that closes before asserting, checks files that are not there and **passes against a store that secures nothing**. *Breaks when:* only the main file is secured — the sidecars carry the same content, including `visibility: internal` items.
- **INV-18** — *moved to ANTS-3761* (export golden-file conformance) — see [ANTS-3761](ANTS-3761-roadmap-export-format.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-19** — *moved to ANTS-3761* (RFC 8785 test-vector conformance) — see [ANTS-3761](ANTS-3761-roadmap-export-format.md). Number retained, never reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this document's sequence is correct.
- **INV-20** — Every item is filed **exactly once**: for each `item` row there is exactly one `element` row with `kind = 'item'` referencing it, and that row's section is the item's filing. The two halves are enforced in **different places**, and the split was found by implementing rather than by drafting: *at most one* is `elem_item_uq`, a partial `UNIQUE` over `element(item_pk) WHERE kind='item'` — it is a uniqueness property of a single column, which DDL expresses exactly; *at least one* is the write path, where `putItem()` inserts the item and its element in **one transaction** — its own `BEGIN IMMEDIATE` when no transaction is open, and the caller's when one is (INV-23, added by [ANTS-3765](ANTS-3765-roadmap-migration-load.md) § 2.3). The invariant is unaffected by which: an enclosing transaction is wider than the one this method would have opened, never narrower, so the two inserts are still atomic together. *Test:* `roadmap_store_schema` asserts three legs — (a) a successful `putItem` leaves exactly one element row, and its section is the one written to; (b) a second `kind='item'` element referencing an already-filed item is **refused by the index**; (c) a failed element insert rolls the item back, so no unfiled item survives. *Breaks when:* filing is left to a `section_id` column on `item`, which is what this replaced — a column cannot express (b) at all, so an item with two element rows in different sections was legal, exported twice, and rebuilt without complaint; or the item and its element are written in separate transactions, which fails (c).
- **INV-14** — `history` is bounded **store-wide, and lossless below the bound**. Both halves are asserted, because each alone certifies the other's failure. (a) Below the cap **no revision is ever evicted**: writing 60 revisions to one item retains all 60. (b) At the cap a `history` write **fails and reports**, and the item write it accompanies still succeeds. *Test:* `roadmap_store_schema`, with the cap **injected** — the production figure is 250 MiB (§ 4) and a test that reached it honestly would take minutes and a disk, so the store takes the bound as a constructor parameter. **The two legs need two different injected caps**, and one value cannot serve both: leg (a) runs with a cap generous enough that 60 revisions stay well below it, and leg (b) with one small enough that a handful of writes crosses it. A single small cap would make leg (a) fail against a *correct* implementation. That injection is a requirement of this invariant, not a testing convenience: a cap reachable only in production is a cap nothing exercises. *Breaks when:* the writer implements a per-item ceiling, which fails (a) by evicting what the model's § 6 says the export exists to preserve; or swallows the at-cap refusal and returns success, which fails (b) and loses a revision invisibly; or hard-codes 250 MB, in which case neither half is testable and the invariant is a comment.
- **INV-21** — The three JSON columns a writer supplies — `lanes`, `evidence`, `extras` — are **reachable through the public API**, and what lands in them is canonical. Added by **ANTS-3767**, which found them unreachable in the shipped surface: `item` had a column for each, `ItemWrite` had a field for none, and `setItemField()`'s allowlist excluded all three, so through the public API they could only ever hold their DDL defaults while every call reported success. That is a defect this document could not see, because § 2.3's canonical-form rule constrains *how* a write is stored and no invariant asserted that the write could happen at all. *Test:* `roadmap_store_schema`, two legs against **raw SQL** rather than a getter — a round-trip through the writer's own idea of the value cannot distinguish a stored value from a default. (a) `putItem()` reaches all three; `extras.tiny = 0.000001` is the ECMAScript fixed-versus-exponential boundary, so that one value is what makes *canonical* assertable rather than merely *written*. (b) `setItemField()` reaches them, canonicalises out-of-order input, keeps provenance per field across the write (INV-10), and refuses a value of the wrong **shape** — `{"a":1}` for `lanes`, `[1]` for `extras`, `[1]` for a string list — each of which parses as JSON. *Breaks when:* a writer binds `QJsonDocument::toJson(Compact)`, which fails leg (a)'s `tiny` and stores non-canonical bytes that fail ANTS-3761's INV-2 diff at the far end of a rebuild, where it is hardest to attribute; or validates a JSON column by parse alone, which fails leg (b) by putting an object in an array column.
- **INV-22** — **`begin()` refuses to nest.** Calling it inside an open transaction fails and reports rather than no-oping, and `commit()`/`rollback()` with none open refuse rather than returning success. Added by [ANTS-3765](ANTS-3765-roadmap-migration-load.md) § 2.3 (its INV-7) along with the four transaction methods themselves; SQLite refuses the nested `BEGIN` itself, so a no-op here would be this project's own invention layered over that refusal. *Test:* `roadmap_store_schema` asserts the second `begin()` returns false and that a subsequent `commit()` still commits the first one's writes. *Breaks when:* it no-ops, so a caller's `commit()` ends a transaction it did not open.
- **INV-23** — **`putItem()` is atomic with or without an enclosing transaction, and never rolls back one it does not own.** With no transaction open it self-commits; with one open it participates, writes nothing durable until the caller commits, and on failure returns `std::nullopt` **without issuing `ROLLBACK`**. ANTS-3765 § 2.3's INV-8, and the one change that spec makes to this one's shipped surface. *Test:* `roadmap_store_schema`, three legs — the standalone case; a `putItem()` inside a rolled-back transaction leaving no item **and** no element row; and a *failing* `putItem()` inside an open transaction after which `inTransaction()` is still true and a later write is still rolled back by the caller. The third is the one that matters: without it, a `putItem()` that aborts the caller's transaction from the inside passes the first two while every later write silently autocommits and persists, and the migration reports a clean partial load. *Breaks when:* the shipped `ROLLBACK` calls are left on the failure paths, or the transaction check reads the connection rather than the store's own flag.
- **INV-24** — **The two JSON-writing methods ANTS-3765 § 2.4 adds store canonical JSON, and the two prose-writing ones store their text verbatim.** `setLegend()` and `addElement()` with `kind='table'` produce the bytes the export would; `setSectionIntro()` and `addElement()` with `kind='narration'` store prose **unchanged**. ANTS-3765's INV-9. *Test:* `roadmap_store_schema` writes an out-of-order legend and table payload and asserts the stored text is sorted and compact, then writes a narration payload containing `{"b":1,"a":2}` as literal prose and asserts it round-trips byte for byte. *Breaks when:* a JSON writer emits indented or non-canonical bytes — the ANTS-3767 failure, one column further along — or the blanket reading is taken and narration prose is canonicalised, which § 2.3 calls undefined rather than merely wasteful.
- **INV-25** — **`addElement()` cannot file an item, `fileItem()` cannot double-file one, and `unfileItem()` unfiles exactly one.** Together they keep `putItem()` and `fileItem()` the only paths by which an item acquires a filing, which is what INV-20 rests on now that a caller other than `putItem()` can write `element` rows. ANTS-3765's INV-10, extended at implementation by `unfileItem()`. *Test:* `roadmap_store_schema` asserts both refusals **by their shape** — the `element` CHECK and `elem_item_uq` refuse both cases too, so a method passing its arguments straight through would also return false, and what distinguishes them is whether the API or the schema spoke — plus that `unfileItem()` leaves a narration row in the same section untouched. *Breaks when:* `addElement()` passes `kind` through and lets the DDL decide, `fileItem()` leans on `elem_item_uq` — turning a reported refusal into a constraint violation that aborts a caller's whole migration — or `unfileItem()` deletes by section rather than by item, destroying payload nothing re-inserts.

## 4. RAM / build cost

**Memory.** § 1 measures the corpus at 4.91 MiB of markdown and names the
command; that figure is not restated here. Qt holds text as UTF-16, so a fully
materialised corpus is **~10 MiB of `QString`** before container and `QVariant`
overhead — call it **12–14 MiB**. Affordable once, unaffordable repeatedly, so:

**Per connection, on top of that: 2 MiB** — SQLite's default page cache, and
the whole reason § 2.5 leaves `cache_size` alone on the interactive profile.
`Access::Bulk` raises it to **16 MiB**, which is affordable precisely because
migration is a one-shot with no concurrent export to starve. `mmap_size` and
`temp_store = MEMORY` are refused outright there; § 2.5 gives the reasoning,
and the short version is that both convert a disk cost into a resident-memory
cost that [ANTS-3761](ANTS-3761-roadmap-export-format.md) INV-12 has already
budgeted away.


- **No query materialises the whole corpus by default.** Every read takes a
  project filter or a `LIMIT`, defaulting to the caller's own project. This is
  a constraint **ANTS-3758 inherits** for the read verbs it owns, not a
  surface this spec defines.
- **No in-process row cache in this spec.** The 100 ms parse cache
  `roadmap_query` uses today (ANTS-1117) is a markdown-parsing cache with
  nothing to cache once reads are SQL. Adding one later needs its own eviction
  policy and its own id.

**Disk.** SQLite stores UTF-8, so the item text is ~4.9 MiB; with indexes, the
JSON columns and `history` **at its migrated size** the store is order
**6–9 MiB** for this corpus. (An earlier draft said 10–15 MiB, which had
borrowed the UTF-16 RAM figure — SQLite does not store UTF-16.)

That figure is the store as it will exist after migration, **not** the store at
the `history` cap below. At the cap it is ~250 MiB by construction. The two
numbers are far apart on purpose: the cap is a backstop against an unbounded
table, and the 6–9 MiB figure is what this corpus actually produces.

**`history` is capped on the store as a whole, not per item, and nothing is
evicted until that bound is reached.** `specs.md` § 4 requires a named cap for
unbounded growth, and `history` is the only table without a natural bound — but
the model's § 6 makes history exported *precisely* because "the store is
untracked and the render is lossy, so that history would otherwise have nowhere
to live — which makes exporting it the point, not an optimisation." A per-item
cap evicting oldest-first destroys the only surviving copy of exactly what that
sentence protects, which is a worse outcome than an oversized export.

So the cap is **250 MiB of `history` across the whole store**, measured as

```sql
SELECT SUM(length(field) + length(coalesce(old_value,''))
                         + length(coalesce(new_value,''))) FROM history;
```

**The measure is pinned because the obvious one is a build-flag dependency.**
Per-table byte size in SQLite comes from `dbstat`, which needs
`SQLITE_ENABLE_DBSTAT_VTAB` — exactly the class of dependency § 2.3 refuses for
JSON1 ("depending on a distro build flag is worse than not depending on the
feature"), and refusing it there while requiring it here would be incoherent.
`length()` over three columns is core SQL, available everywhere, and it counts
the payload rather than the page overhead — an under-count by a stable factor,
which is the right error direction for a backstop. The writer keeps a running
total rather than re-summing per write; the sum above is what INV-14 asserts
against and what a fresh store recomputes at open.

The rule is then:

- **Below the bound, nothing is ever evicted.** No item has a revision ceiling.
- **At the bound, writes to `history` stop and report** — the same failure mode
  § 2.5 gives a write that cannot take the lock, and for the same reason. The
  item write itself still succeeds; it is the audit row that is refused. A
  silently dropped revision is indistinguishable from one that never happened.
- **What to evict is deliberately not decided here.** Reaching the bound is the
  trigger for a decision made against a measured growth rate, which does not
  exist yet, rather than a rule guessed now and inherited forever.

**Settled by the standard's author, 2026-07-30**, over per-item middle-eviction
and over unbounded growth. The honest caveat is that this defers *which*
revisions go rather than answering it — accepted because the deferral is bounded
by a number rather than open-ended, and because the number is far away: 250 MiB
is ~50× the entire markdown corpus (§ 1), and migration backfills at most one
history row per item (ANTS-3757), so only live editing accumulates against it.
The cap is a real backstop against an unbounded table, not a limit this corpus
is expected to meet.

**Indexes.** SQLite does **not** auto-index foreign keys, so a join over one
would scan — **unless that column already leads a unique index or primary key**,
in which case a second index costs writes and buys nothing. Applying that one
rule to every foreign key in § 2.3, rather than listing from memory:

| Declared | Not declared — already leads something |
|---|---|
| `section(parent_id)` | `item(project_id)` — leads `UNIQUE (project_id, id_fold)` |
| `element(item_pk)` | `section(project_id)` — leads `UNIQUE (project_id, slug)` |
| `relationship(src_pk)` | `element(section_id)` — leads `UNIQUE (section_id, position)` |
| `relationship(dst_pk)` | `id_prefix(project_id)` — leads its composite PK |
| `citation(project_id)` | `feedback_ref(item_pk)` — leads its composite PK |
| | `history(item_pk)` — leads `UNIQUE (item_pk, changed_at, seq)` |
| | `citation(item_pk)` — leads `cite_item_uq` |

`item(section_id)` is absent from both columns because the column itself is
gone (§ 2.3); `element(item_pk)` is what an item's filing is now looked up
through, and it is declared.

`citation(project_id)` is in the **declared** column and its siblings are not,
because `cite_doc_uq` is `WHERE doc_path IS NOT NULL` — it cannot serve a lookup
on the item-anchored rows, so unlike every other entry on the right the leading
column does not cover the FK.

`citation(item_pk)` needed checking rather than asserting, because
`cite_item_uq` is a **partial** index — `WHERE item_pk IS NOT NULL` — and a
partial index is only usable when the query's constraints imply the index's.
Measured on SQLite 3.53.2: `EXPLAIN QUERY PLAN SELECT * FROM citation WHERE
item_pk = 7` reports `SEARCH citation USING INDEX cite_item_uq (item_pk=?)`, so
the implication is recognised and the separate index is genuinely redundant.

**Build.** One new library target, `roadmapstore`, linking `Qt6::Core` and
`Qt6::Sql`. Three new feature-test **directories**, all joining the existing
`test_core` bundle rather than adding binaries (§ 6). A new **external** runtime
dependency (§ 2.1) across five packaging carriers and CI.

## 5. Out of scope

- **Migration** — reading the ten existing markdown roadmaps into this schema, including pass-headings status normalisation and bulk id allocation. Tracked by **ANTS-3757**.
- **The published render, the backup repo's push cadence, and the fate of `roadmap_query` / `roadmap_log` / `RoadmapDialog`.** Tracked by **ANTS-3758**.
- **The health-check suite** (duplicate ids, a feedback file citing an id no project owns, a spec `Status` disagreeing with its bullet). Scheduling and per-check behaviour are **ANTS-3758**; this spec provides only the schema they query. Two further checks belong there explicitly, so their absence here is a decision rather than an oversight: the **acyclicity** of `splits-from` / `blocked-by` / `duplicate-of` / `supersedes` (the model's § 6 makes it a whole-store property, which no per-row constraint can express), and the model's INV-1 **second leg** — comparing the *committed* export against a fresh export of the live store. [ANTS-3761](ANTS-3761-roadmap-export-format.md)'s INV-1 implements only the re-export-equality leg, which the model itself calls insufficient alone. (INV-1 *here* is a tombstone — the invariant moved with the export half.)
- **Multi-machine sync.** The export lands in a git repo and git is the sync mechanism; the store itself is single-machine. This is a **permanent exclusion**, not deferred work — a store synchronised at the SQLite level is a different product, and the export exists precisely so it is not needed.
- **Post-cutover id allocation.** `id_prefix` holds the high-water mark (§ 2.3) and the model's § 7.1 makes the store its owner, but *handing out* the next id — monotonicity, atomicity under `BEGIN IMMEDIATE`, and what a caller gets when two projects allocate at once — is **ANTS-3758**'s, with the write verbs that would call it. This spec provides the column and the constraint that a retired id is never reissued (the high-water mark outlives the row); it defines no allocation call, so none is tested here. Migration's *bulk* allocation is separately ANTS-3757's.
- **Full-text search over bodies.** A **permanent exclusion** for this spec: the store's job is to hold the model and round-trip it, and search is a read surface, which ANTS-3758 owns. Not deferred, so no id is owed here — if search is wanted, it is that spec's decision whether FTS5 or a `LIKE` scan serves it.

## 6. Tests

Feature tests under `tests/features/`, label `features;fast`:

| Directory | Covers |
|---|---|
| `roadmap_store_schema/` | INV-6, INV-7, INV-8, INV-10, INV-11, INV-14, INV-17, INV-20, INV-21, INV-22, INV-23, INV-24, INV-25 |
| `roadmap_store_identity/` | INV-3, INV-4 |
| `roadmap_store_concurrency/` | INV-15, INV-16 |

**These are three new feature-test DIRECTORIES, not three new binaries.** All
three add their `test_*.cpp` to **`test_core`**'s `SOURCES` list — the bundle
`tests/features/README.md` assigns to session / config / packaging, which is
where project-state persistence belongs (the store is a data layer, not a dialog
subject). Per that README's step 4, no `add_executable` is added: ANTS-1217
consolidated 141 standalone binaries into seven bundles to bound build-time RAM,
and three new binaries would reverse it. `roadmap_store_concurrency` needs two
processes for INV-15, and it gets them with `fork(2)` **inside** the bundle
test rather than as a separate target, so the exception the README allows for
process isolation is not needed here.

All **thirteen** invariants this spec retains are covered; none is a grep-only
check. The **eight** tombstoned above — INV-1, 2, 5, 9, 12, 13, 18 and 19 — are
covered by [ANTS-3761](ANTS-3761-roadmap-export-format.md) § 6, whose own
coverage table accounts for all eight.

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
- **[`docs/standards/dependencies.md`](../standards/dependencies.md)** — § 4's minimum-supported-floors table gains SQLite at **3.31** (generated columns, § 2.3), with its mandatory *Guard* column, and § 6's version map gains the new runtime dependency. That standard is what § 2.3's runner reasoning rests on, so a floor added here and not there is a floor nothing sweeps. The floor is a real one and not a formality: it is what makes `id_fold`'s generated column available, and both constrained runners already clear it.
- **`.github/workflows/release.yml`** — carries its **own** apt list for the AppImage build, separate from `ci.yml`'s.
- **`tools/ci-parity.sh`** — carries a **third** apt list, for the `qt62_image` container it runs the Qt 6.2 compile guard in. Its own comment requires it to be kept "in lockstep with `ci.yml`'s `qt62-baseline` … step", and `dependencies.md` § 6 lists it as a carrier for exactly that reason. A package added to `ci.yml` and not here desynchronises the parity harness silently — CI stays green and the harness stops reproducing it, which is the one failure that tool exists to prevent.
- **`ROADMAP.md`** — the ANTS-3756 bullet still describes this spec as owning "export serialisation … the export's record types, field order, sort collation and encoding". That moved to ANTS-3761 at the split. Cross-reference only, not a contract — but it is the entry point anyone looking for this work reads first.
- **`README.md`** — "the only thing it needs to run is Qt6" becomes false by the same argument as `CLAUDE.md`, and the per-distro build-dependency lines each gain a package.
- **[`mcp-caches.md`](../standards/mcp-caches.md)** — gains a **short non-cache note, not a row in the inventory table**. That table's columns (Spec / Storage / Keyed by / Relocation profile) and its checklist are explicitly for adding a new *path-keyed cache*, so a non-cache entry has no honest cell to fill — forcing one in is how the file would come to list `roadmap.sqlite` among things a GC sweep may delete. The note records that `roadmap.sqlite` is not a cache, is not path-keyed, and must never be swept; its never-shadow invariant applies to the `project.root` column instead (INV-8).
- **[`roadmap-data-model.md`](../standards/roadmap-data-model.md)** — its § 9 assigns the schema, the export record types and concurrency to "the spec". Once this ships, the **schema** bullet and the **store half** of the concurrency bullet point here. The **export** bullet points at [ANTS-3761](ANTS-3761-roadmap-export-format.md), which claims it in its own § 7; and the auto-publish cadence bundled into the concurrency bullet is **ANTS-3758**'s (§ 5). Stated at this granularity because both halves of the split previously claimed the same § 9 bullet, and two live specs asserting authority over one standard sentence is the conflict the split was supposed to remove.
- **`CLAUDE.md`** — the "Qt6 is the only runtime dep" line becomes **false** and must be amended (§ 2.1): the QSQLITE driver links system `libsqlite3`.
- **[`docs/subsystems.md`](../subsystems.md)** — gains the `roadmapstore` lane. The module map moved there in ANTS-1292; `CLAUDE.md` carries only a pointer, and `indie_review_partition` derives one review lane per entry from that file, so a lane added to `CLAUDE.md` instead would never be reviewed.
- **`CHANGELOG.md`** — new store, user-invisible until ANTS-3758 lands the render.
- **[ANTS-3765](ANTS-3765-roadmap-migration-load.md) amends this document rather than merely citing it** (2026-08-01). **Twenty-three methods** land on `RoadmapStore`'s public surface: four transaction methods (`begin`/`commit`/`rollback`/`inTransaction`), ten writers (`setSectionIntro`, `updateSection`, `addElement`, `fileItem`, `unfileItem`, `clearSectionElements`, `setLegend`, `raiseIdHighWater`, `clearItemField`, and a four-argument `setItemField` overload taking the provenance value), and nine readers (`findItem`, `readItem`, `listItems`, `findSection`, `readSection`, `idPrefixFor`, `idHighWater`, `maxHistorySeq`, `access`). The four-argument `setItemField()` is an **overload**, so INV-10 is untouched. One shipped behaviour changes: `putItem()` no longer unconditionally opens its own transaction, and no longer issues `ROLLBACK` when it does not own one — INV-20's wording above and INV-23 both carry it. Its four store invariants are folded in here as **INV-22–25**, and are ANTS-3765's own INV-7–10: this document already has an INV-7, INV-8 and INV-10 meaning something else, so they are renumbered rather than imported, and `roadmap_store_schema`'s test names use these numbers.
- **[ANTS-3782](ANTS-3782-roadmap-section-provenance.md) amends this document rather than merely citing it** (2026-08-01; the amendment was applied under ANTS-3766's id and moved to ANTS-3782 when that spec was split out the same day). `section` gains one nullable column, `source_path` — project-root-relative, `NULL` for the live roadmap — written into the `CREATE TABLE` above rather than added by `ALTER`, and **with no `kSchemaVersion` bump**: no store is reachable from user-facing code, `createSchema()` has no branch between version 0 and its own, and the export goldens carry `"schema":1`. ANTS-3782 § 2.1 carries that argument in full and is its only statement. `SectionRow` and `readSection()` — on this document's public surface, declared by ANTS-3765 § 2.4 — gain the matching `std::optional<QString> sourcePath`, without which the column would be write-only and ANTS-3782 INV-14 could not read it back. **One writer joins that surface too, and the reader alone would not have been enough:** `setSectionSource(qint64, const std::optional<QString> &, QString *)`, declared alongside `setSectionIntro()` in ANTS-3765 § 2.4 and following its precedent — a separate setter, because ANTS-3765 § 2.6 resolves a section and then writes the fields that differ, so the write must reach an **existing** row and an INSERT-only `addSection()` cannot. Without it the column has a reader, a renderer and no way to be set, which is the ANTS-3767 defect one column along: `item`'s `lanes`, `evidence` and `extras` each had a column and no writer, so every call reported success while the columns held their DDL defaults. That makes **twenty-four** methods on this surface rather than the twenty-three the bullet above counts. **No invariant here changes** — the column is nullable, unindexed, outside every `UNIQUE`, not a foreign key (so it adds no row to § 4's index table), and named by no invariant in *this* document; ANTS-3782's INV-14 is what constrains its value — so nothing is renumbered and nothing needs a `specs.md` § 5.5 annotation. ANTS-3765 § 2.6 writes the value; **ANTS-3781** owns the upgrade path this exposed as missing, and § 2.3's corrected sentence now points there.

- **[ANTS-3796](ANTS-3796-section-record-completeness.md) amends this document rather than merely citing it** (2026-08-03). `section` gains one more column, `position INTEGER NOT NULL` — document order among *this project's* sections, project-wide rather than per-parent — written into the `CREATE TABLE` above rather than added by `ALTER`, and **with no `kSchemaVersion` bump**, on the identical argument ANTS-3782 made one column earlier and for the last time: the freedom expires at ANTS-3758's cutover, and ANTS-3781 owns what follows. It is deliberately **not** `UNIQUE (project_id, position)`, which reads like the obvious constraint: `element` gets away with `UNIQUE (section_id, position)` only because `clearSectionElements()` lets the migration rewrite a whole sequence, and sections cannot be cleared because `element.section_id` and item filing reference them — so a re-run swapping two sections would collide mid-update, and SQLite offers `DEFERRABLE INITIALLY DEFERRED` for foreign keys only. Distinctness is a writer's obligation within a run's plan (ANTS-3796 INV-4), not a DDL constraint. **Two signatures on this surface change and one method joins it.** `addSection()` and `updateSection()` both take `position` as a required parameter rather than gaining a setter — `setSectionIntro()` and `setSectionSource()` are setters because their columns are nullable and were added to a shipped signature, whereas this column is `NOT NULL` with no default, so an insert omitting it could not succeed and a setter would be unreachable; in `addSection()` it is placed **before** the defaulted `parentId` so every existing call site is a compile error rather than a silent rebinding. `SectionRow` and `readSection()` gain the matching `int position`, without which the column would be write-only and ANTS-3765 § 2.6's "written only if it differs" could not see a section that *moved*. The new method is `listSections(qint64, QString *) const`, the section counterpart of `listItems()`: `findSection()` resolves one slug and `readSection()` is a point lookup, so neither can produce the SET a sort key applies to, and without it every caller would reach past the typed reader into raw SQL. That makes **twenty-five** methods on this surface. A free function `sectionOrderLess(const SectionRow &, const SectionRow &)` lands beside `SectionRow` in `roadmapstore.h` — a free function because it compares two rows and touches no store state; ANTS-3796 § 2.2 owns the `(position, slug)` key it applies, this document owns its home. **No invariant here changes** — the column is outside every `UNIQUE`, not a foreign key, adds no row to § 4's index table, and is named by no invariant in *this* document.

## 8. Open questions

None outstanding. The three this spec carried are closed:

- **Does `id_fold` derive in the schema or the writer?** **Closed — in the
  schema**, as a `GENERATED` column (§ 2.3), verified against SQLite rather than
  reasoned about. The question framed the generated form's unfalsifiability as a
  cost; it is the point.
- **Which project's export carries a cross-project relationship?** **Closed —
  the source project's**, decided in
  [ANTS-3761](ANTS-3761-roadmap-export-format.md) § 2.3 along with the record
  shape (`dst_project` names the far side). It moved there with the export half;
  the schema's part is `relationship`'s three target forms (§ 2.3).
- **How does the writer locate the `claude-config` checkout?** **Not this
  spec's** — it is an export-deployment question, and
  [ANTS-3761](ANTS-3761-roadmap-export-format.md) § 5 excludes it permanently in
  favour of ANTS-3758, which owns the backup surface. The store never touches
  that repo.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 1 | 2026-07-30 | 2 (design + contract, grounding + cross-doc) | 7 / 11 / 10 / 10 / 0 | All 38 verified, 0 dismissed, all draft defects. Both lanes independently falsified § 2.1's engine justification — Qt's QSQLITE driver links **system** `libsqlite3` and ships in a separate package, so a third-party runtime dep does enter the graph and `CLAUDE.md`'s "Qt6 is the only runtime dep" needs amending, not defending. § 2.4 was a list of constraints on a format rather than a format: no record shape, no discriminator, surrogate rowids serialised (which makes INV-1 unachievable on any store that has ever deleted a row), and 4 of 8 record types unordered. Rewritten with nine record types, a worked example line, every freedom pinned, and a variable-length numeric sort that survives `PASS-43-5-B`. Four invariants would have certified a broken store — INV-8 asserted the exact failure its *Breaks when* forbade, INV-7 named a `QStandardPaths` constant that resolves to the wrong directory on this project, INV-1+2 were jointly satisfied by a column-lossy writer, INV-10 by a writer ignoring provenance. INV-11..13 added. Enum CHECKs, FK pragma, `busy_timeout`, export transaction, `history` cap, indexes, and five packaging carriers where the draft named one. **Doc grew 321 → 511 lines while being fixed** — noted per Phase 2 as a size signal, not re-partitioned mid-run. |
| 2 | 2026-07-30 | 2 (same partition, cold) | 9 / 12 / 12 / 10 / 0 | Findings rose against loop 1 and **~75% were collateral from loop 1's own fixes** — so the loop was stopped and the cause addressed rather than dispatching loop 3. Root cause: § 2.4 guaranteed byte-identity via a hand-written table of ~20 serialisation freedoms, and every freedom pinned created new places for the document to contradict itself. The clincher was external: `QJsonObject` sorts keys, so the pinned field order was unproducible with Qt's own JSON classes. **Resolved by decision (user, 2026-07-30): adopt RFC 8785 (JCS)** — key order, whitespace, escaping, numbers and encoding now come from a published standard, and this spec keeps only the file-level rules JCS cannot cover. Also fixed: loop 1's edit had inserted prose *inside* a `sql` fence (17 fence lines, odd) so the schema rendered as one code block — `doc_integrity` and `spec_lint` both pass an unbalanced-fence document, which is why it shipped; a fence-parity check is now part of the post-fix pass. `id_prefix` gained an export record (its high-water mark was otherwise lost on every rebuild, reissuing live ids); INV-7's cache-prefix test was inverted and passed for a store in the cache; `relates-to` normalised on a rowid in contradiction of its own section; the relationship and citation UNIQUEs never fired because SQLite treats NULLs as distinct; `BEGIN IMMEDIATE`, temp+rename, and a SQLite 3.38 floor added. INV-14 added. |
| 3 | 2026-07-30 | 2 (same partition, cold) | 8 / 12 / 12 / 12 / 1 | **Converged by cap.** Collateral outnumbered draft defects for the second loop running — the skill's stop trigger — and the doc has grown 321 → 683 lines while being fixed, so the run exits here rather than looping a fourth time. Both lanes independently verified the RFC 8785 delegation is characterised correctly, which is the loop-2 decision holding. Fixed this loop: the surrogate-key rule was four names short in two places and is now stated as a rule rather than a list; `id_fold` was declared never-exported while every reference record emitted it; `root` was `NOT NULL` and unexportable, blocking the rebuild leg outright; INV-11 asserted a CHECK over a JSON object, which SQLite cannot express; the coverage table said "thirteen" of fourteen invariants and omitted INV-14; INV-5's seeds tested neither of the sort rules loop 2 added; `source` was treated as defaultless when the model defaults it. **The SQLite floor fork was closed against evidence**: both branches of "move the runners" are foreclosed — `release.yml`'s `ubuntu-22.04` is a deliberate glibc-2.35 pin, and `dependencies.md` § 6 forbids bumping `qt62-baseline` — so the store now depends on no JSON1. Two conflicts with the standard are **surfaced, not resolved**: `sort_order`'s obligation tier, and `history` eviction, which as drafted would destroy the only copy of what the model's § 6 says the export exists to preserve. Deferred tail filed as **ANTS-3760**. |
| 3-split | 2026-07-30 | none — no reviewer dispatched | — | **Provenance row, not a review.** Acting on loop 3's split recommendation (user-approved): the export half — serialisation, record types, file-level ordering, id sort and the export write path — moved to **[ANTS-3761](ANTS-3761-roadmap-export-format.md)**, taking INV-1, 2, 5, 9, 12 and 13 with it. Those numbers are **tombstoned in place here, never reflowed** (`specs.md` § 5.5), so this spec's sequence has gaps by design. What stayed is the database: engine and its dependency cost, location, schema, connection pragmas. Also folded in from ANTS-3760's tail while editing: `synchronous = FULL` and a stated busy-timeout failure mode (a primary store owes durability a cache does not), the `user_version == 0` creation-race discriminator, the duplicated corpus measurement deleted from § 4, and the disk figure corrected — it had borrowed the UTF-16 RAM number, and SQLite stores UTF-8. **This spec's three loops do not transfer to ANTS-3761**, which runs the gate from loop 1 on its own bytes; and this half is itself now materially smaller than what those loops reviewed, so its next gate run is against a changed document. |
| 4 | 2026-07-30 | 1 (store lane, cold, counterpart also read) | 5 / 6 / 6 / 6 / 0 | First gate on the post-split document. Two **seam failures** where ANTS-3761 emitted fields this schema had no column for: cross-project `rel` (no `dst_project`/`dst_id_fold`, and the CHECK forbade an unresolved edge outright) and per-project `citation` (no `project_id`, so a doc-anchored row had no path to a project and `cite_doc_uq` collided across projects). Both fixed here. `section_id NOT NULL` was justified by a migration default the model's § 3.3 does **not** contain — it defaults only `kind` and `source` — so this spec now supplies the synthetic root section itself. `id_parses` was a boolean conflating genuinely off-grammar ids with synthesised `PASS-N-M[-S]` ones, which the model's § 7.1 calls real ids; replaced by a three-value `id_origin`. § 4 carried **two** `**Disk.**` paragraphs, the second restating verbatim a figure the first retracts. Added: 0600 on the store and both WAL sidecars (INV-17), an `export_slug` charset CHECK (it is interpolated into a path), timestamp CHECKs so the export cannot normalise what the store holds loosely, `NOT NULL DEFAULT` on the JSON columns, and INV-15/16 for the creation race and busy-timeout policy — § 2.5 previously had no invariant and no test at all. |
| 5-fold | 2026-07-30 | none — no reviewer dispatched | — | **Decision + fold-in row, not a review.** The two conflicts loops 3 and 4 surfaced were **settled by the standard's author**, and ANTS-3760's remaining verified tail was folded in without re-review (it is already verified; re-deriving it would cost a full dispatch). *`sort_order`* → `roadmap-data-model.md` § 4.1's row becomes `derived` and § 3.1's write tier drops it, so the standard now says what this schema does; § 2.3's "open against the standard" paragraph is retired. *`history` eviction* → capped **store-wide at 250 MiB with nothing evicted below it**, chosen over per-item middle-eviction and over unbounded growth; at the cap the `history` write fails and reports while the item write it accompanies still succeeds. INV-14 now asserts **both** halves and requires the cap to be **injectable**, because a bound reachable only in production is a bound no test exercises. Folded from the tail: a self-relationship `CHECK`; the four same-project rules moved to the write path with SQLite named as the reason they cannot be DDL; INV-8 bound to `QFileInfo::canonicalFilePath()` and given a third leg for its empty-string-on-missing-path behaviour, which `root TEXT UNIQUE` would otherwise fuse into a single shared project; `citation(item_pk)` dropped as redundant; two stale `§ 2.4` self-citations repointed at ANTS-3761. **Four claims were verified against SQLite 3.53.2 rather than reasoned about** — a partial index *is* used for `item_pk = ?`, a subquery in a `CHECK` *is* prohibited, the self-edge `CHECK` fires, and a generated `id_fold` both folds and **refuses a direct INSERT**. The last of those closed § 8's third open question: `id_fold` is now `GENERATED ALWAYS AS (lower(id)) VIRTUAL`, which makes a non-folding writer unreachable rather than merely tested against, and sets a real SQLite floor of **3.31** that both constrained runners clear. § 8 is consequently empty — its other two questions belong to ANTS-3761 and ANTS-3758, which have since decided them. |
| 5 | 2026-07-30 | 2 (schema + invariants-as-tests, grounding + seam; cold, loop log withheld) | 4 / 12 / 7 / 11 / 0 | 34 verified, 0 dismissed, 33 fixed and **1 surfaced** (below). Origin split: ~4 collateral from the 5-fold edit, ~30 draft defects — draft defects dominate, so the loop was the right remedy, but all four collateral items came from one edit whose blast radius I under-swept (the 250 MiB cap left `6–9 MiB` "at its cap" standing, the unit drifted MB/MiB, and INV-14's single injected cap could not satisfy both its own legs). **Two invariants would have certified a broken store.** INV-17 said to "force a WAL checkpoint so `-wal` and `-shm` exist" — verified on SQLite 3.53.2 that a *write* creates the sidecars, a checkpoint does not, and closing the last connection deletes them, so the recipe asserted on absent files and passed against a store securing nothing. INV-11's per-enum sweep omitted `id_origin`, a closed enum with a `CHECK` of its own. **Two schema rules were unsatisfiable:** `section.title` is `NOT NULL` while § 2.3 specified a synthetic root section with "no title", which stops the first migration insert; and `relates-to` normalisation demanded the lower-sorting endpoint be `src_pk` when `src_pk` is `NOT NULL REFERENCES item` and the model's INV-4 permits an unresolved far endpoint — now scoped to both-endpoints-resolved. The 250 MiB cap had **no defined measure**, and the obvious one (`dbstat`) is a build-flag dependency of exactly the class § 2.3 refuses for JSON1; pinned to a `length()` sum instead. Grounding: "sixteen helper **libraries**" was a wrong noun on a right count — 16 comments annotate *sources inside one library*, and the file declares 8 `add_library` targets; "the MCP verb layer, which may be a second process" is contradicted by `remotecontrol.cpp:2860` (verbs run on the GUI thread). Seam with ANTS-3761: both documents said **six** invariants were tombstoned where there are **eight**; `provenance` lacked the `DEFAULT` the sibling asserts it has; `element.payload` was to be canonicalised as JSON though it is prose for `kind='narration'`; ANTS-3761's exhaustive absent/null table omitted `history.old`/`new` and mis-filed `item.source`; its INV-2 join keys omitted the two columns that lead the store's own unique indexes. Also: § 9 of the standard was claimed by **both** halves of the split; `tools/ci-parity.sh` was missing from § 7 though `dependencies.md` § 6 requires it in lockstep; three "new bundles" would have reversed ANTS-1217's consolidation and are now three directories joining `test_core`; `user_version` mismatch behaviour defined; `export_slug`'s CHECK narrowed to what its own derivation can emit. **Surfaced, not fixed:** `item.section_id` duplicates the `element` row's filing with no rule naming which wins — the same defect this spec rejects for `sort_order`, and both copies are exported, so no invariant catches the drift. It is a design decision affecting both specs and the export record shape, so it goes to the author rather than being resolved here. |
| 6-decision | 2026-07-30 | none — no reviewer dispatched | — | **Decision row, not a review.** Loop 5's surfaced CRITICAL, answered by the standard's author the same day and on the same grounds as `sort_order`: **`item.section_id` is removed.** An item's filing is its `element` row, which already carries both the section and the position, so the column was a second encoding of one fact with no authority named — the exact defect § 5 of the model settles against `sort_order`, and accepting it there while reproducing it here would have been incoherent. What the `NOT NULL` column bought is replaced by **INV-20** (exactly one `kind='item'` element per item), which is strictly stronger: the column never forbade an item with *two* element rows, or with an element row in a section other than its own `section_id`, and both states exported and rebuilt without complaint. Swept with it: the write-path constraint list (the same-project `item.section_id` rule is gone, INV-20 takes its place), the index table (`item(section_id)` removed; `element(item_pk)` is now how filing is looked up), § 6's coverage count eleven → twelve, and on the export side ANTS-3761's `item` record loses its `section` field and the surrogate list loses `item.section_id`. |
| 7-impl | 2026-07-30 | none — implementation, not a review | — | **Implementation row, written by the implementer** (`/cold-eyes` writes only review rows). `src/roadmapstore.{h,cpp}` + `ants_roadmapstore_lib` built to § 2.3 / § 2.5; 15 feature tests across `roadmap_store_identity/` and `roadmap_store_schema/`, all green, **all eight verified to go RED under the exact mutation their own *Breaks when* clause names** — enums widened, `UNIQUE` moved to `id`, `elem_item_uq` dropped, sidecar `chmod` removed, the empty-canonical guard bypassed, provenance replaced rather than merged, normalisation keyed on the rowid, and the cap made unreachable. A test that stays green against its named break tests nothing, so this is the proof, not the pass. **One spec clause was proved wrong and is amended**: INV-20 was written entirely as a write-path rule on the grounds that it compares two rows, but *at most one filing* is a uniqueness property of a **single column** — it is now the partial index `elem_item_uq ON element(item_pk) WHERE kind='item'`, and only *at least one* stays in the write path. **Two defects the compiler could not see** were caught by `/write-code`'s trigger table and are recorded because both were invisible on this host: `INSERT … RETURNING` needs **SQLite 3.35** and would have silently raised § 2.3's stated **3.31** floor by four releases (replaced with `lastInsertId()`); and `putItem()`'s same-project check read *outside* its own transaction, a read-then-write race, now inside `BEGIN IMMEDIATE`. Unchanged from the spec as gated: every other DDL constraint, all four pragmas, the `user_version` creation discriminator, and the `length()`-sum history measure. |
| 8-impl | 2026-07-30 | none — implementation, not a review | — | **Implementation row (concurrency bundle), written by the implementer.** `tests/features/roadmap_store_concurrency/` completes § 6's third directory — 3 tests joining `test_core`, INV-15 forking two openers released together through a pipe, INV-16 in two legs. Suite **3108/3108**. **Three § 2.5 clauses amended against evidence, and none of the three was reachable by reading.** (1) The DDL is written **without** `CREATE TABLE IF NOT EXISTS`, which § 2.5 prescribed: `user_version` already guarantees the loser never reaches the DDL, so a `CREATE TABLE` that runs against an existing table means the discriminator has regressed and must fail loudly — `IF NOT EXISTS` would mask exactly the failure INV-15 exists to catch. (2) **`PRAGMA journal_mode = WAL` is not covered by `busy_timeout`** — it takes an EXCLUSIVE lock *below* the busy handler, and the forked openers failed the open on **18 of 25 runs** with the deadline already at 5000. `enableWal()` supplies the retry SQLite declines to, bounded by the same constant rather than a second one; `busy_timeout` also moved to the head of the pragma list. (3) INV-16's recipe assumed waiting out the real deadline; the blocked connection's is shortened to 100 ms instead, and the 5000 ms value gets a leg of its own. **A named break that does NOT redden is recorded rather than quietly dropped:** removing the `busy_timeout` pragma entirely leaves leg (a) green, because Qt's QSQLITE plugin sets 5000 ms itself (`QSQLITE_BUSY_TIMEOUT`) — so that leg asserts the *effective* deadline, and a constant drift 5000 → 100 is what proves it has teeth. The pragma stays: a durability contract should not rest on an undocumented driver default. One API addition, `createdSchema()`, because winner and loser are identical after the fact and INV-15 otherwise has no observable. The two remaining named breaks went RED as written — `IF NOT EXISTS` creation (both children report `created`) and a `putItem()` that swallows its failed `BEGIN IMMEDIATE`. |
| 9-impl | 2026-07-30 | none — implementation, not a review | — | **Implementation row (connection profile), written by the implementer, prompted by a user-requested review of a sibling project.** RetroDB (`/mnt/Games/Scripts/Linux/RetroDB/`) has been through "database is locked" several times and its changelog records what actually fixed it; § 2.5 now carries what transfers. **What transferred:** `journal_size_limit` (we had nothing bounding WAL growth); two access profiles, `Interactive` at the existing 5000 ms and `Bulk` at 30000 ms for migration/export, which is RetroDB's figure and leaves INV-16 untouched since both still fail and report; a 16 MiB page cache on `Bulk` only; `PRAGMA optimize` on close. **What did not, and the refusals are the load-bearing part:** `synchronous = NORMAL` (their DB is re-scrapeable, § 1 makes ours primary), and `mmap_size` / `temp_store = MEMORY`, both of which convert a disk cost into resident memory and would break [ANTS-3761](ANTS-3761-roadmap-export-format.md) INV-12's 4 MiB export budget — the tests now assert their **defaults**, so a later performance sweep cannot enable them without meeting that budget first. **One real defect found, and it was ours, not theirs:** `createSchema()` took `BEGIN IMMEDIATE` on *every* open merely to read `user_version`, so every ordinary open queued behind any active writer — measured at the full 5000 ms deadline, and the concurrency suite ran 5118 ms where it now runs 115 ms. A shared-lock read outside the transaction fixes it; the authoritative read inside `BEGIN IMMEDIATE` is untouched, so INV-15 is unaffected. **One hypothesis was falsified mid-fix and the code was reverted rather than kept:** a read-before-write guard in `enableWal()` was added on the theory that re-asserting `journal_mode` was what contended. Deleting it left the contention test green — SQLite takes the exclusive lock only when the mode actually *changes* — so the guard changed no observable behaviour and was removed. **Measurement corrected a claim on both sides:** `journal_size_limit` is connection-scoped, not file-level (set, reconnect, read back: `-1`), which is the opposite of how RetroDB classified it; there it was moved to a once-per-boot init path and is consequently not in force on the connections serving requests. Reported to the user, not edited into that project. Two new tests; all four named breaks RED, including the sibling's misclassification. |
| 10-impl | 2026-07-31 | none — implementation, not a review | — | **Implementation row (ANTS-3767, the JSON-column write path), written by the implementer.** Found by ANTS-3757 checking its § 2.1.1 store-field table against this surface rather than against its prose, and **re-verified against shipped source before the fix**, not carried on the bullet's word. `item.lanes`, `item.evidence` and `item.extras` were unreachable: a DDL column each, no `ItemWrite` field, and all three absent from `setItemField()`'s allowlist, so the public API could only ever leave them at `'[]'`/`'[]'`/`'{}'` while reporting success. Three documents assumed otherwise — this spec's § 2.3 stated a canonical-form rule "at the write path" for a write path that could not be reached, `roadmap-data-model.md` § 4.1 makes `lanes`/`evidence` first-class, and ANTS-3761's export emits all three per item. **The gap survived five review loops because no invariant asserted reachability** — § 2.3 constrains how a write is stored, never that it can happen — so the remedy is INV-21 rather than a wider § 2.3. Fixed: `ItemWrite` gains the three fields; `putItem()` binds them canonically (an empty list writes `'[]'` explicitly rather than leaning on the DDL default, so these columns have one producer, not two); `setItemField()` accepts them with a **shape** check, since `{"a":1}` parses cleanly and is not a lane list. `canonicalJson()` widened `QJsonObject` → `QJsonValue` because two of the three are arrays — source-compatible, all four existing call sites pass an object. Both INV-21 legs were shown RED first against the shipped surface (`[]`/`{}` on leg (a), `field not writable: lanes` on leg (b)). Suite 3136/3136. |
| 11-impl | 2026-08-01 | none — implementation, not a review | — | **Implementation row (ANTS-3765's store surface), written by the implementer.** Twenty-three methods land on `RoadmapStore` for the migration load half, and § 7's new bullet lists them; INV-22–25 above are that spec's INV-7–10, renumbered because this document already has an INV-7, INV-8 and INV-10 meaning something else — a collision worth recording, since `roadmap_store_schema` would otherwise have carried two tests called `Inv7*` testing unrelated things. **One shipped decision changed and INV-20's wording with it:** `putItem()` opened its own transaction unconditionally, which made "one project is one transaction" unexpressible in both directions — SQLite refuses a nested `BEGIN`, so a wrapper's first `putItem()` failed and the migration wrote nothing, while dropping the wrapper left a failure at item 400 of 600 with 399 committed. It now participates in an open transaction and, critically, issues `ROLLBACK` on its failure paths **only when it owns** the transaction: left unchanged, one bad item would abort the caller's transaction from the inside, after which every later write autocommits and persists while the report says the load was clean — INV-1 of that spec inverted with nothing observing it. **Three methods were added that its § 2.4 had not declared**, each performing an operation its own prose mandates: `readSection()` (sections had no reader for the "write only what differs" comparison), `idPrefixFor()` (§ 2.8's "the project's existing `id_prefix` row" is unreachable through a reader that takes the prefix as an argument) and `unfileItem()` (an item still in source whose stored section the plan no longer carries kept its element row, and `fileItem()` then refused it, aborting the project on an edit as ordinary as deleting a heading). All four invariants were shown RED against the mutation their own *Breaks when* clause names before the implementation was restored — a no-oping `begin()`, the shipped `ROLLBACK` left in place, an indented `setLegend()` plus a narration payload canonicalised, and both filing refusals delegated to the DDL. INV-25's assertion is on the refusal's **shape** rather than its existence, because the schema refuses those two cases as well: a method passing its arguments through would also return false, and what the invariant is actually about is which layer spoke. Suite 3136/3136 unchanged plus 5 new tests. |
