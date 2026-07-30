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

**`section_id` is `NOT NULL`** because § 3.1 makes `section` a write
obligation and § 3.3 gives migration a default — an item filed nowhere is not a
state the model has.

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
  CHECK ((dst_pk IS NULL) != (dst_path IS NULL)),
  UNIQUE (type, src_pk, dst_pk, dst_path)
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

#### Record types

Every line is one JSON object with a `t` discriminator. Field order within a
line is the order given here — **not** § 2.3's column order, which contains
surrogates.

```jsonl
{"t":"meta","schema":1,"project":"ants-terminal","root":"/mnt/Games/Scripts/Linux/Ants_Terminal","exported":"2026-07-30"}
{"t":"legend","status":"in-progress","wording":"In progress (active commit work…)"}
{"t":"section","slug":"performance-2","title":"Performance","level":3,"parent":null,"intro":null}
{"t":"item","id":"ANTS-1234","id_parses":true,"section":"performance-2","status":"shipped","kind":"perf","headline":"…","layman":"…","source":"…","priority":2,"visibility":"public","milestone":null,"resolution":"…","body":"…","created":"2026-07-30","last_modified":"2026-07-30","shipped":"2026-07-30","lanes":["vt"],"evidence":[],"extras":{},"provenance":{"kind":"defaulted"}}
{"t":"element","section":"performance-2","position":0,"kind":"item","ref":"ants-1234"}
{"t":"rel","type":"blocked-by","src":"ants-1234","dst":"ants-1200"}
{"t":"citation","src":"ants-1234","file":"src/vtparser.cpp","symbol":"VtParser::feed"}
{"t":"feedback_ref","item":"ants-1234","file":"Vestige_Ants_MCP_Feedback.md"}
{"t":"history","item":"ants-1234","at":"2026-07-30T09:15:00Z","field":"status","old":"planned","new":"shipped"}
```

`id` is emitted verbatim; `id_fold` is **derived, never stored in the export**
— the reader recomputes it, so the two can never disagree.

#### Every freedom, pinned

| Freedom | Pinned to |
|---|---|
| Record-type order | `meta`, `legend`, `section`, `item`, `element`, `rel`, `citation`, `feedback_ref`, `history` — all nine, in that order |
| `legend` order | by `status`, in § 7.3's declared enum order |
| `section` order | by `slug`, byte order |
| `item` order | by the id sort below |
| `element` order | by `(section slug, position)` |
| `rel` order | by `(type, src, dst)`, byte order |
| `citation` / `feedback_ref` order | by `(src, file, symbol)` / `(item, file)`, byte order |
| `history` order | by `(item, at, field)`, byte order |
| Key order within a line | as listed in the record types above |
| Nested object key order | `provenance` in § 2.3 column order; `extras` in source-document order (its 292 keys have no canonical order, so source order is the only stable one) |
| `element.payload` table shape | `{"header":[…],"rows":[[…],…]}` |
| Absent vs null | a field with no value is **omitted**; `null` never appears except where the record type shows it explicitly (`parent`, `intro`, `milestone`) |
| Empty string | is a value, not an absence — emitted |
| Booleans | `true` / `false`, never `0` / `1` |
| Unicode | UTF-8, no BOM, no `\uXXXX` escaping above U+007F |
| Control characters | the two-char forms JSON defines (`\n`, `\t`, `\r`, `\b`, `\f`, `\"`, `\\`); everything else below U+0020 as `\u00XX` lowercase hex. `/` is never escaped |
| Numbers | integers only; no floats anywhere in the model |
| Timestamps | dates `YYYY-MM-DD`; `history.at` is `YYYY-MM-DDTHH:MM:SSZ`, UTC, second precision, always `Z` |
| Line ending | `\n`, including on the final line |
| Whitespace | none — no indentation, no space after `:` or `,` |

#### Id sort order

Items sort by a **variable-length numeric-segment tuple**, because no simpler
rule survives the corpus:

1. Split the id into alphabetic and numeric runs (`ANTS-1234` → `("ants", 1234)`; `PASS-43-5-B` → `("pass", 43, 5, "b")`).
2. Compare run by run: alphabetic runs by **byte order** on the folded text (never locale collation, which varies by `LC_COLLATE` and would make the export machine-dependent); numeric runs by value.
3. A shorter tuple sorts before a longer one with the same prefix (`PASS-43-5` before `PASS-43-5-B`).
4. **Tie-break on the raw `id` bytes.** Required, not defensive: zero-padding is write-side only (`roadmap-format.md` § 3.5.1), so `CL-9` and `CL-0009` produce the identical tuple and the sort would otherwise be non-total.
5. Quarantined ids (`id_parses = 0`) sort **last**, among themselves by raw `id` bytes.

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

- **INV-1** — Exporting the live store, rebuilding a fresh store from that export, and re-exporting produces byte-identical files for every project. *Test:* `tests/features/roadmap_store_roundtrip/` — export, rebuild into a temp store, re-export, `cmp` each pair. *Breaks when:* any § 2.4 freedom is left unpinned; seed the fixture with two items whose insertion order differs from their id order.
- **INV-2** — The export is complete: every store row **and every column of it** appears in the export. *Test:* `roadmap_store_roundtrip` — after rebuild, per-table `COUNT(*)` matches for all nine tables, **and** a full column-wise diff of every row matches. *Breaks when:* a writer drops a whole column — say `provenance` — which round-trips byte-identically and preserves every row count, so INV-1 and a count-only INV-2 both pass on a lossy store. The column-wise leg is what makes the pair meaningful; the model's own INV-1 demands the same second leg.
- **INV-3** — Item identity is case-folded within a project: inserting `Sh-1` then `SH-1` into one project raises a uniqueness violation, and the same pair in two different projects does not. *Test:* `roadmap_store_identity`. *Breaks when:* `UNIQUE (project_id, id_fold)` is written against `id`.
- **INV-4** — An off-grammar id is stored verbatim with `id_parses = 0` and is never rewritten. *Test:* `roadmap_store_identity` inserts `[Cl9]`; assert `id` round-trips exactly and no dash is inserted. *Breaks when:* a writer normalises ids on the way in.
- **INV-5** — Export item order follows § 2.4's numeric-segment sort and is **total**. *Test:* `roadmap_store_roundtrip` seeds `ANTS-9`, `ANTS-10`, `CL-9`, `CL-0009`, `PASS-43-5`, `PASS-43-5-B`, `PASS-9-1` and one quarantined id, and asserts the exact emitted order. *Breaks when:* the writer sorts lexically (`ANTS-10` lands before `ANTS-9`); or splits at the last hyphen (`PASS-9-1` lands after `PASS-43-5`); or omits the raw-bytes tie-break, leaving `CL-9` and `CL-0009` unordered — a non-total sort makes INV-1 fail intermittently, which is the worst way for it to fail.
- **INV-6** — `relates-to` is stored once, normalised on stable identity. Writing A→B then B→A yields exactly one row, and the stored direction survives a rebuild. *Test:* `roadmap_store_schema` writes both directions and asserts one row; `roadmap_store_roundtrip` asserts the direction is unchanged after export-rebuild-export. *Breaks when:* normalisation keys on `item_pk` — rowids are reassigned by the rebuild, so the direction can flip and the re-export differs, defeating the invariant's own purpose.
- **INV-7** — The resolved store path is under `GenericDataLocation + "/ants-terminal"` and never under any cache location. *Test:* `roadmap_store_schema` asserts on the **resolved path at runtime** — it must equal `QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/ants-terminal/roadmap.sqlite"`, and **neither cache root may be a prefix of it** — the resolved path must not start with `writableLocation(CacheLocation)`, nor with `writableLocation(GenericCacheLocation) + "/ants-terminal"` (where the project's real caches live). The direction matters: `~/.cache/ants-terminal/roadmap.sqlite` is *not* a prefix of the cache root, so the reversed comparison passes for exactly the placement this invariant forbids. A source-grep for `CacheLocation` near the store is a secondary guard only. *Breaks when:* the path is composed from a constant defined elsewhere — a grep then sees one hit and the location is decided somewhere it never looked, so a grep-only test passes against a store sitting in the cache.
- **INV-8** — A project is keyed on its **canonical** root: two paths that canonicalise to the same directory are one project, and a genuinely different root is a different project. *Test:* `roadmap_store_schema` — (a) insert via a symlinked path and via the real path, assert **one** `project_id`; (b) insert two genuinely distinct roots, assert two. *Breaks when:* `root` is stored unresolved, which makes (a) yield two rows. Case (a) is the one that matters: `mcp-caches.md`'s never-shadow rule applied to rows instead of files, and asserting *two* there would certify exactly the bug it forbids.
- **INV-9** — Every export write path acquires `ConfigWriteLock`, and aborts loudly when it cannot. *Test:* `roadmap_store_concurrency` — hold the lock, attempt an export, assert it returns an error and **wrote no bytes**; release, assert it then succeeds. *Breaks when:* the writer treats `!acquired()` as permission to proceed unprotected. Phrasing matters here: `flock` is advisory, so a non-cooperating writer *can* interleave — the testable claim is about our writer, not about the file.
- **INV-10** — `provenance` is per field, in both directions: editing `headline` through the store sets `provenance.headline` to `asserted` **and** leaves `provenance.kind` untouched. *Test:* `roadmap_store_schema` asserts both halves. *Breaks when:* provenance is stored per item — or when the writer never updates provenance at all, which a one-sided "kind is unchanged" assertion would happily certify.
- **INV-11** — Every closed enum is rejected at the storage layer, not merely documented: inserting a `status`, `visibility`, `element.kind` or `relationship.type` outside its set, or a `priority` outside 1–5, fails. *Test:* `roadmap_store_schema` attempts one invalid insert per enum and asserts each is refused. *Breaks when:* the enums are written as SQL comments, which is how the first draft of § 2.3 had them.
- **INV-12** — The export writer streams: exporting a store holding the whole corpus never materialises the file in memory. *Test:* `roadmap_store_roundtrip` runs the export under a peak-RSS ceiling well below the export's own byte size. *Breaks when:* the writer builds one `QString` and writes it at the end — which passes every other invariant here.
- **INV-13** — No surrogate key appears in the export. *Test:* `roadmap_store_roundtrip` greps the produced `.jsonl` for the key names `item_pk`, `project_id`, `section_id`, `element_id`, `rel_id`, `history_id`; expects zero matches. *Breaks when:* a writer serialises rows straight from `SELECT *`, which is the natural implementation and silently breaks INV-1 on any store that has ever deleted a row.

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
- **The health-check suite** (duplicate ids, a feedback file citing an id no project owns, a spec `Status` disagreeing with its bullet). Scheduling and per-check behaviour are **ANTS-3758**; this spec provides only the schema they query.
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
- **`.github/workflows/ci.yml`** — three jobs install an explicit apt list, including the `qt62-baseline` job. Without the SQL module every feature test below fails at runtime in CI.
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
