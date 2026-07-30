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
projects, 3,940 bullet-form and checkbox items plus 144 pass-heading items**.
Total markdown across all ten roadmap files is **4.91 MiB**
(`du -cb */[Rr]oadmap.md`), which is the order of the text the store must hold.

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

**Why this does not breach "Qt6 is the only runtime dep"** (`CLAUDE.md`):
`Qt6::Sql` ships with Qt and links the SQLite amalgamation Qt already vendors.
No third-party package enters the dependency graph. The cost is a Qt module and
a plugin, not a new project.

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

`XDG_DATA_HOME`, **not** `XDG_CACHE_HOME`. `mcp-caches.md`'s inventory is a
list of things that may be deleted and rebuilt; this file is not one, and it is
added to that document as an explicit non-cache row (§ 7) so nobody adds it to
a GC sweep later.

The store holds every project, so it is **not** path-keyed and
`mcp-caches.md`'s shadow rule does not apply to the file itself. It applies to
the `project` rows inside it — see INV-8.

### 2.3 Schema

One table per entity, all projects in each. "One table per project", as
ANTS-3753 originally asked, is implemented as **one table keyed on project**;
the reconciliation is recorded in that bullet (nine-way stitches and nine-way
migrations fight the stated goal of maintaining the whole thing from any
project).

```sql
CREATE TABLE project (
  project_id   INTEGER PRIMARY KEY,
  root         TEXT NOT NULL UNIQUE,   -- canonical absolute path
  name         TEXT NOT NULL,
  legend       TEXT                    -- JSON: {status_value: project_wording}
);

CREATE TABLE item (
  item_pk      INTEGER PRIMARY KEY,    -- surrogate; relationship targets use it
  project_id   INTEGER NOT NULL REFERENCES project(project_id),
  id           TEXT NOT NULL,          -- verbatim, as authored
  id_fold      TEXT NOT NULL,          -- lowercased; the identity key
  id_parses    INTEGER NOT NULL,       -- 0 for the quarantined off-grammar ids
  status       TEXT NOT NULL,
  headline     TEXT NOT NULL,
  layman       TEXT,
  kind         TEXT NOT NULL,
  source       TEXT,
  priority     INTEGER,
  visibility   TEXT NOT NULL DEFAULT 'public',
  milestone    TEXT,
  resolution   TEXT,
  body         TEXT,
  section_id   INTEGER REFERENCES section(section_id),
  sort_order   INTEGER NOT NULL,
  created      TEXT, last_modified TEXT, shipped TEXT,   -- ISO 8601
  lanes        TEXT,                   -- JSON array
  evidence     TEXT,                   -- JSON array
  extras       TEXT,                   -- JSON object: the § 4.3 tail
  provenance   TEXT NOT NULL,          -- JSON object: field -> § 7.7 value
  UNIQUE (project_id, id_fold)
);
```

`id_fold` carries § 7.1's case-insensitive identity: `Sh-1` and `SH-1` are one
item. `id` keeps what the author wrote, because § 3.5.1 makes ids append-only
and rewriting one breaks every citation.

**`extras` and `provenance` are JSON columns, not tables.** `extras` holds a
tail of **over 280 distinct keys** measured across the corpus, almost all
appearing once or twice; a key-value table would be 280 sparse joins to
reconstruct one item, and SQLite's JSON1 functions query it directly when a
report needs to. `provenance` is per field by § 7.7, so it is naturally an
object keyed by field name.

```sql
CREATE TABLE section (
  section_id  INTEGER PRIMARY KEY,
  project_id  INTEGER NOT NULL REFERENCES project(project_id),
  slug        TEXT NOT NULL,
  title       TEXT NOT NULL,
  level       INTEGER NOT NULL,
  intro       TEXT,
  parent_id   INTEGER REFERENCES section(section_id),
  sort_order  INTEGER NOT NULL,
  UNIQUE (project_id, slug)
);

-- § 5's ordered element list. An item element points at item_pk; a narration
-- or table element carries its payload inline.
CREATE TABLE element (
  element_id  INTEGER PRIMARY KEY,
  section_id  INTEGER NOT NULL REFERENCES section(section_id),
  position    INTEGER NOT NULL,
  kind        TEXT NOT NULL,          -- 'item' | 'narration' | 'table'
  item_pk     INTEGER REFERENCES item(item_pk),
  payload     TEXT,                   -- narration prose, or JSON table
  UNIQUE (section_id, position)
);

CREATE TABLE relationship (
  rel_id      INTEGER PRIMARY KEY,
  type        TEXT NOT NULL,          -- § 6's six types
  src_pk      INTEGER NOT NULL REFERENCES item(item_pk),
  dst_pk      INTEGER REFERENCES item(item_pk),   -- item target
  dst_path    TEXT,                                -- document target
  CHECK ((dst_pk IS NULL) != (dst_path IS NULL))
);

CREATE TABLE history (
  history_id  INTEGER PRIMARY KEY,
  item_pk     INTEGER NOT NULL REFERENCES item(item_pk),
  changed_at  TEXT NOT NULL,
  field       TEXT NOT NULL,
  old_value   TEXT,
  new_value   TEXT
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
  symbol      TEXT,
  CHECK ((item_pk IS NULL) != (doc_path IS NULL))
);
```

`relates-to` is symmetric per § 6 and is **stored once**, with `src_pk <
dst_pk` enforced at write. Storing both directions would make the export's byte
equality depend on insertion order.

### 2.4 Export serialisation

One file per project, **JSON Lines**, in the private `claude-config` repo:
`roadmap-export/<project-name>.jsonl`.

JSON Lines rather than one JSON document because it gives one item per line,
which is the review granularity ANTS-3753 asked for — a changed item is a
one-line diff.

Byte-equality needs every degree of freedom removed, so the writer pins all of
them:

| Freedom | Pinned to |
|---|---|
| Record order | `project`, then `section`, then `item`, then relationships |
| Item order | by `(prefix, numeric)` of `id_fold` — see below |
| Object key order | the order declared in § 2.3, not insertion or alphabetical |
| Absent vs null | absent. A field with no value is omitted entirely |
| Unicode | UTF-8, no BOM, no `\uXXXX` escaping above U+007F |
| Numbers | integers only; no floats anywhere in the model |
| Line ending | `\n`, including on the final line |
| Whitespace | none — no indentation, no space after `:` or `,` |

**Item ordering is by `(prefix, numeric)` tuple, not lexical.** Lexical sorting
puts `ANTS-10` before `ANTS-9`, and the corpus contains genuinely unpadded ids
(`CL-9`), so a padded-string sort is not available either. The tuple sort is
stable across every id shape including the synthesised `PASS-N-M`. Quarantined
off-grammar ids (`id_parses = 0`) sort last, by raw byte order, since they have
no numeric part to extract.

### 2.5 Concurrency

Two writers exist: the Ants GUI process and the MCP verb layer, which may be a
second process (`CLAUDE.md` — the live binary runs from a home-drive copy, and
a Quake instance plus a regular window is a normal state).

- **SQLite in WAL mode** — one writer, concurrent readers, which is the access
  shape here.
- **`ConfigWriteLock`** (`src/configbackup.h`) wraps the export write. It is
  the project's existing RAII `flock(2)` guard with a 5-second deadline, and
  `tests/features/concurrent_writer_lock/` already locks its behaviour. The
  export is a whole-file rewrite, which is exactly the read-modify-write shape
  that guard exists for. Reusing it beats inventing a second locking scheme
  (`CLAUDE.md` § 3 — reuse before rewriting).

## 3. Invariants

- **INV-1** — Exporting the live store, rebuilding a fresh store from that export, and re-exporting produces byte-identical files for every project. *Test:* `tests/features/roadmap_store_roundtrip/` — export, rebuild into a temp store, re-export, `cmp` each pair. *Breaks when:* any § 2.4 freedom is left unpinned; seed the fixture with two items whose insertion order differs from their id order.
- **INV-2** — The export is complete: every store row appears in it. *Test:* same feature test — after rebuild, per-table `COUNT(*)` matches the source store for all eight tables. *Breaks when:* a table is added to § 2.3 and not to the writer; an empty export trivially satisfies re-export equality alone, which is why this is separate from INV-1.
- **INV-3** — Item identity is case-folded within a project: inserting `Sh-1` then `SH-1` into one project raises a uniqueness violation, and the same pair in two different projects does not. *Test:* `roadmap_store_identity`. *Breaks when:* `UNIQUE (project_id, id_fold)` is written against `id`.
- **INV-4** — An off-grammar id is stored verbatim with `id_parses = 0` and is never rewritten. *Test:* `roadmap_store_identity` inserts `[Cl9]`; assert `id` round-trips exactly and no dash is inserted. *Breaks when:* a writer normalises ids on the way in.
- **INV-5** — Export item order is `(prefix, numeric)`: a project holding `ANTS-9`, `ANTS-10` and `CL-9` exports them in that numeric order, not lexically. *Test:* `roadmap_store_roundtrip`. *Breaks when:* the writer sorts on the string.
- **INV-6** — `relates-to` is stored once. Writing A→B then B→A yields exactly one row. *Test:* `roadmap_store_schema`. *Breaks when:* the `src_pk < dst_pk` normalisation is dropped; the export then differs by insertion order and INV-1 fails downstream.
- **INV-7** — The store file lives under `XDG_DATA_HOME` and no code path places it under `XDG_CACHE_HOME`. *Test:* source-grep — `grep -rn "roadmap.sqlite" src/` returns only call sites resolving `XDG_DATA_HOME` / `QStandardPaths::AppDataLocation`, and zero matching `CacheLocation`. *Breaks when:* the store is added to a cache sweep.
- **INV-8** — A project is keyed on its canonical absolute root, so a relocated project reads as a new project rather than inheriting another's rows. *Test:* `roadmap_store_schema` inserts two projects whose roots differ only by a symlink hop; assert two `project_id`s. *Breaks when:* `root` is stored unresolved — this is `mcp-caches.md`'s never-shadow rule applied to rows instead of files.
- **INV-9** — A concurrent export write is serialised: a second writer holding no lock cannot interleave. *Test:* `roadmap_store_concurrency`, modelled on `tests/features/concurrent_writer_lock/`. *Breaks when:* the export write skips `ConfigWriteLock`.
- **INV-10** — `provenance` is per field and a field's value is never silently promoted to `asserted`: editing `headline` through the store leaves `kind`'s provenance unchanged. *Test:* `roadmap_store_schema`. *Breaks when:* provenance is stored per item.

## 4. RAM / build cost

**Memory.** The corpus is 4.91 MiB of markdown across ten roadmaps, so a
fully-materialised corpus is on the order of **6–8 MiB** once parsed into rows
(text dominates; the fixed columns are small). That is affordable once and
unaffordable repeatedly, so:

- **No query materialises the whole corpus by default.** Every read verb takes
  a project filter or a `LIMIT`, defaulting to the caller's own project.
- **The export writer streams** — one item per line, never a whole-corpus
  string in memory. This is a correctness requirement as well as a budget one:
  a 5 MiB `QString` built by concatenation reallocates repeatedly.
- **No in-process row cache in this spec.** The 100 ms parse cache
  `roadmap_query` uses today (ANTS-1117) is a markdown-parsing cache with
  nothing to cache once reads are SQL. Adding one later needs its own eviction
  policy and its own id.

**Build.** One new library target, `roadmapstore`, linking `Qt6::Core` and
`Qt6::Sql`. One new feature-test bundle. No new external package. The AppImage
gains the `libqsqlite.so` driver plugin.

## 5. Out of scope

- **Migration** — reading the ten existing markdown roadmaps into this schema, including pass-headings status normalisation and bulk id allocation. Tracked by **ANTS-3757**.
- **The published render, the backup repo's push cadence, and the fate of `roadmap_query` / `roadmap_log` / `RoadmapDialog`.** Tracked by **ANTS-3758**.
- **The health-check suite** (duplicate ids, a feedback file citing an id no project owns, a spec `Status` disagreeing with its bullet). Scheduling and per-check behaviour are **ANTS-3758**; this spec provides only the schema they query.
- **Multi-machine sync.** The export lands in a git repo and git is the sync mechanism; the store itself is single-machine. This is a **permanent exclusion**, not deferred work — a store synchronised at the SQLite level is a different product, and the export exists precisely so it is not needed.
- **Full-text search over bodies.** SQLite FTS5 would serve it, and no caller has asked. Permanent exclusion until one does.

## 6. Tests

Feature tests under `tests/features/`, label `features;fast`:

| Directory | Covers |
|---|---|
| `roadmap_store_schema/` | INV-6, INV-8, INV-10 |
| `roadmap_store_identity/` | INV-3, INV-4 |
| `roadmap_store_roundtrip/` | INV-1, INV-2, INV-5 |
| `roadmap_store_concurrency/` | INV-9 |

INV-7 is a source-grep, run in `roadmap_store_schema`'s script half.

Per the project convention (`CLAUDE.md`, `testing.md`), each test must be
verified to **fail against pre-implementation source** before the
implementation is restored. For INV-1 that means a deliberately unpinned
writer — emit object keys in insertion order and confirm the round-trip
comparison fails — because a round-trip test written against an already-correct
writer passes for reasons it never checked.

## 7. Cross-doc impact

- **`CMakeLists.txt`** — `Sql` added to the Qt6 `COMPONENTS` list; new `roadmapstore` target.
- **`packaging/`** — AppImage recipe bundles `sqldrivers/libqsqlite.so`. A missing driver fails at runtime, not at link time.
- **[`mcp-caches.md`](../standards/mcp-caches.md)** — gains a row recording that `roadmap.sqlite` is **not** a cache, is not path-keyed, and must never be added to a GC sweep. Its never-shadow invariant applies to the `project.root` column instead (INV-8).
- **[`roadmap-data-model.md`](../standards/roadmap-data-model.md)** — its § 9 currently says the schema, the export record types and concurrency are the spec's; once this ships, those bullets point here.
- **`CLAUDE.md`** — the "Qt6 is the only runtime dep" line stays true but now spans a module it did not before; the module map gains `roadmapstore`.
- **`CHANGELOG.md`** — new store, user-invisible until ANTS-3758 lands the render.

## 8. Open questions

- **Does the export belong in `claude-config` at repo root or under a subdirectory per project?** § 2.4 assumes `roadmap-export/<project-name>.jsonl`. The alternative — one directory per project — matters only if a project ever exports more than one file, which nothing in this spec does.
- **Should `history` be capped?** § 6 of the data model makes it exported, so it grows without bound and is the one table that can dominate the export's size. No cap is specified here because no rate is known yet; ANTS-3757's migration backfill is what will produce the first real number.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
