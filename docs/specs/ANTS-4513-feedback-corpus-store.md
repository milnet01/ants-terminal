<!-- ants-spec-format: 1 -->
# ANTS-4513 — Move the feedback corpus into the roadmap store

**Status:** accepted (2026-11-04).
**Kind:** feature.
**Source:** ROADMAP ANTS-4513 (user-request-2026-08-19).
**Pairs with:** ANTS-3781 (the schema-upgrade ladder this adds a rung to),
ANTS-4622 (the last table added to the store, and the model for this one).

**Layman:** The shared feedback files move into the same database as the
roadmap. Sessions file and triage feedback exactly as before. Two sessions
writing at once can no longer lose each other's text, and the files stay on
disk as a read-only copy anyone can open.

## 1. Goal

Every `*_Ants_MCP_Feedback.md` finding lives in the roadmap store. The store
is the record; each file is rewritten from it after every change. Writes from
concurrent sessions are serialised, and "what is un-triaged anywhere" is one
query rather than a parse of every file. Contributors and the maintainer call
the same verbs with the same arguments as today.

## 2. Problem

1. **Concurrent writes can lose text.** Every `feedback_log` write reads the
   whole file, edits it in memory and saves it through `fbAtomicWrite`
   (`src/remotecontrol_feedback.cpp`), a `QSaveFile`. Nothing locks the file
   between the read and the save. Two sessions appending at once: the later
   save drops the earlier one's finding.
2. **The un-triaged view is a parse of every file.**
   `RemoteControl::buildFeedbackPendingBlock` (`src/remotecontrol_state.cpp`)
   globs the corpus and runs `FeedbackFile::parse` on each file on every
   `session_orient`. The corpus is 30 files and 854,071 bytes
   (`ls *_Ants_MCP_Feedback.md | wc -l`; `cat … | wc -c`, in
   `Ants_MCP_Feedback_Files`, 2026-11-04).
3. **The files have no history.** They sit outside every git repository. A
   bad write cannot be undone.
4. **Six ops exist only to keep files tidy.** `compact_resolved` stops files
   growing; `compact_shipped`, `prune_tracking`, `append_tracking` and
   `migrate_v2` serve format version 1; `set_format_version` stamps a version.
   No file in the corpus is version 1: `grep -L 'ants-mcp-feedback: 2'` over
   it lists none.

## 3. Scope decisions (agreed with the user)

- **The files stay, as a read-only export** (user, 2026-11-04). The store is
  primary. Each file is rewritten from the store after every write, so it
  stays readable when the MCP server is down.
- **A project that has never filed is registered on its first filing**
  (user, 2026-11-04). There is one storage path. **How this spec honours
  it:** "registered" means a `feedback_reporter` row (§ 4.1), created the
  first time a verb touches that reporter's file. It is not a `project` row:
  two reporters (`Charls_Site`, `Games`) have no roadmap, and `project`
  registration needs one (`roadmap_migrate` refuses `no_roadmap`).
- **Store the text, index the fields** (author's call, § 8). Each reporter's
  document is stored verbatim; finding rows are derived from it.

## 4. Surface

### 4.1 Schema — rung 5

`kSchemaVersion` (`src/roadmapstore.h`) goes from 4 to 5. Rung 5 joins
`upgradeLadder()` and runs the DDL below. As with `kMessageTableDdl`, the
statements are constants shared by `createSchema()` and the rung, so a new
store and an upgraded one hold the same schema text (ANTS-3781 INV-8).

```sql
CREATE TABLE feedback_reporter (
  reporter_id    INTEGER PRIMARY KEY,
  export_path    TEXT NOT NULL UNIQUE,   -- canonical absolute path of the .md
  content        TEXT NOT NULL,          -- the whole document, verbatim
  revision       INTEGER NOT NULL DEFAULT 1 CHECK (revision >= 1),
  export_sha256  TEXT,                   -- of the bytes last written; NULL until then
  created_at     TEXT NOT NULL CHECK (created_at GLOB
    '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z'),
  updated_at     TEXT NOT NULL CHECK (updated_at GLOB
    '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z')
);

CREATE TABLE feedback_finding (
  reporter_id    INTEGER NOT NULL REFERENCES feedback_reporter(reporter_id),
  ordinal        INTEGER NOT NULL,       -- 0-based, document order
  heading        TEXT NOT NULL,          -- the ### line's text
  heading_line   INTEGER NOT NULL,       -- 1-based line in content
  entry_heading  TEXT NOT NULL DEFAULT '',  -- enclosing contributor-block heading
  filed_on       TEXT CHECK (filed_on IS NULL OR filed_on GLOB
                   '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'),
  triage         TEXT NOT NULL CHECK (triage IN
                   ('untriaged','awaiting','closed','mapped','untagged')),
  ids            TEXT NOT NULL DEFAULT '[]',  -- JSON array, Proposed ID order
  PRIMARY KEY (reporter_id, ordinal)
);

CREATE INDEX idx_feedback_finding_triage ON feedback_finding(triage);
```

**Every `feedback_finding` column is a function of `content`.** The rows for
a reporter are rebuilt from its `content` on every write. So no column holds
anything the document does not, and the document alone round-trips.

**The classifier is `FeedbackFile::parse`, not a new one.** A `###` block
`parse` counts as un-triaged is `untriaged`; one carrying the awaiting marker
is `awaiting`; an `n/a` closure is `closed`; one with roadmap ids is `mapped`;
one `parse` lists in `suspectedUntagged` is `untagged`. Today `parse` returns
no per-finding record, so `ParseResult` gains one: a list filled in `parse`'s
v2 loop by the same tests that build `delta`, `awaiting` and
`suspectedUntagged`. Each entry carries the heading, its line, the triage
value, the ids and the enclosing contributor-block heading. `filed_on` is that
heading's leading `YYYY-MM-DD`, NULL when it has none. The row builder copies
the list and classifies nothing itself.

`ids` holds the ids as written, whatever their prefix. Their status is
resolved live from the roadmap, as `feedback_query` does today
(`rlResolveForeignFeedbackIds`). It is never stored.

`feedback_ref` is not touched (§ 9).

### 4.2 Which reporter a call means

Path resolution is unchanged: `resolveFeedbackPath`
(`src/remotecontrol_workspace.cpp`) turns `caller_cwd` and `path` into a file
path, with every refusal it has today. The `export_path` key is the canonical
path of that file's directory joined with its file name, so a file not yet
created keys the same as it will once it exists (`canonicalFilePath()` is
empty for a missing file). One reporter is one file.

### 4.3 Import — on first touch

When a verb resolves an `export_path` that has no row:

- **The file exists** → it is imported inside a `BEGIN IMMEDIATE`
  transaction, on every path that imports: the write ops, `feedback_query` and
  the pending scan. `content` is the file's bytes, decoded as UTF-8. A file
  whose bytes do not survive decoding and re-encoding refuses `not_utf8`, and
  nothing is written: invalid UTF-8, or a leading byte-order mark, which
  `QString::fromUtf8` drops (Qt 6, measured 2026-11-04). A file whose format
  marker is below 2 refuses `not_v2`: `parse` builds no per-finding list for
  it, and `migrate_v2` is retired. The finding rows are derived,
  `export_sha256` is the file's hash, and the file is not rewritten.
- **The file does not exist** → `append_finding` creates the row with
  `FeedbackFile::skeleton` as `content`, then proceeds. This is today's
  create-on-append, moved into the store. Every other op answers as today for
  a missing file.

Once a row exists, every op and `feedback_query` treat the reporter as
existing, whether or not its file is on disk.

`buildFeedbackPendingBlock` imports every file it globs that has no row.
Import is the only way a row comes into being, so a file is imported once.

### 4.4 The write path

Every surviving write op (§ 4.6) runs one shape:

```
BEGIN IMMEDIATE
  row     = reporter for export_path (import per § 4.3 if absent)
  if the file exists and sha256(file) != row.export_sha256 → ROLLBACK, refuse export_drift
  // a missing file is not drift: the write below recreates it
  text    = <the op's existing text edit>(row.content, args)   // unchanged code
  if text == row.content → ROLLBACK, answer changed:false        // today's no-op skip
  rebuild feedback_finding rows from text
  UPDATE feedback_reporter SET content=text, revision=revision+1, updated_at=now
  chmod u+w export_path if it exists; write text via QSaveFile; chmod 0444
  UPDATE feedback_reporter SET export_sha256 = sha256(text)
COMMIT
```

The text edits are the functions the ops call today: `FeedbackFile::assignId`
for `assign_id` and `assign_id_batch`, `FeedbackFile::setTitle`, and the
`append_finding` renderer. Their behaviour and their invariants in ANTS-1962,
ANTS-3447 and ANTS-4646 carry over unchanged, because the same code runs on
the same text.

`BEGIN IMMEDIATE` takes the store's write lock before the read, which is
what § 2 item 1 lacks. The store already sets `busy_timeout` and WAL in
`applyPragmas`.

The export is written inside the transaction, so a second writer cannot
render between this commit and this file write. The owner-write bit comes
back first because `QSaveFile::open` refuses a file that is not writable
("Existing file … is not writable", Qt 6, measured 2026-11-04). A `dry_run`
stops before the first `UPDATE` and rolls back.

### 4.5 Drift — a hand-edited export

A file whose bytes no longer hash to `export_sha256` has been edited outside
the verbs. Writes to that reporter refuse `export_drift` and change nothing,
neither the rows nor the file. Reads answer from the store and add
`export_drift: true`.

The maintainer settles it with `feedback_log op:"reimport"`, which needs
`confirm: true`. It replaces `content` with the file's bytes and rebuilds the
rows, as § 4.3 does. Without `confirm` it refuses `confirm_required` and
reports the byte counts of both versions. Keeping the store's version needs
no op: delete the file, and the next write recreates it.

### 4.6 The ops after this

| Op | After |
|---|---|
| `append_finding`, `assign_id`, `assign_id_batch`, `set_title` | write path § 4.4 |
| `reimport` | new, § 4.5 |
| `compact_resolved`, `compact_shipped`, `prune_tracking`, `append_tracking`, `migrate_v2`, `set_format_version` | refuse `retired`; nothing is written |

A `retired` refusal names the op and says the store made it unnecessary.

`feedback_query` answers from the store: it runs `FeedbackFile::parse` on the
row's `content` and returns the reply it returns today. `include_tracking`
keeps its meaning.

`session_orient`'s `feedback_pending` keeps its fields and its gate. Which
reporters have pending findings, and the awaiting and untagged counts, come
from the `feedback_finding` rows of the reporters whose files this scan
globbed. `total_pending_lines` comes from `parse` on the
`content` of those reporters only, not of every file. A file whose import
refuses is listed under `import_refused` with its name and refusal code, and
is not counted.

### 4.7 Errors with no file fallback

A store that does not open refuses `store_failed`. Neither verb falls back to
reading or writing the file: that would be the second storage path § 3 rules
out. In `session_orient`, `feedback_pending` then carries `store_failed: true`
and no counts, and orient itself does not fail.

### 4.8 Reload

Both verbs are registered in `mcp::registerProjectScopedVerbs()`, so
`ants-mcpd` serves them. A rebuilt `ants-mcpd` reaches a client on its next
reconnect, with no terminal relaunch. They hold no running state and so have
no stop/start control.

**The schema bump does need one relaunch.** The first `ants-mcpd` built at
version 5 upgrades the store. A terminal built before then refuses a store
newer than itself (`createSchema()`'s `version > kSchemaVersion` arm,
`newerSchemaError`), and `src/roadmapdialog.cpp` opens the store. So the
terminal's Roadmap window fails until the terminal is relaunched. This is
unavoidable: ANTS-3756 § 2.3 refuses a downgrade by design. Ship it when a
relaunch is due anyway.

## 5. Invariants

- **INV-1 — Import loses nothing.** For every corpus file, the `content` of
  its imported row equals the file's bytes, and the export written from it is
  byte-identical to the original.
  *Test:* `tests/features/feedback_store/` imports a copy of each fixture file
  into a temporary store, writes it out, and compares bytes. The fixtures
  include a contributor block with prose before its first finding, a `###`
  block with no Proposed ID line, and a compaction stub.
  *Breaks when:* the import normalises line endings or a trailing newline, or
  re-renders from the derived rows instead of storing the text.

- **INV-2 — The rows classify as `parse` does.** For each imported document,
  the findings with `triage` `untriaged` or `awaiting` are exactly the
  findings `FeedbackFile::parse` puts in the delta and in `awaiting[]`, and
  `untagged` matches `suspectedUntagged`.
  *Test:* the same bundle, comparing the derived rows with `parse` on each
  fixture, plus a fixture whose Note bullet holds an `ANTS-` id.
  *Breaks when:* the row builder reads the Proposed ID with its own regex and
  takes an id from a Note line.

- **INV-3 — Rows follow `content` after every write.** After any
  `feedback_log` write, rebuilding the rows from the stored `content` yields
  the stored rows.
  *Test:* the bundle runs each surviving op, then rebuilds and compares.
  *Breaks when:* an op updates `content` but skips the row rebuild.

- **INV-4 — `feedback_query` answers as before.** For an imported reporter,
  its reply equals the reply `FeedbackFile::parse` gives on the same file's
  bytes, apart from `etag`.
  *Test:* the bundle runs the reply builder over each fixture both ways:
  through the store, and through `parse` on the file.
  *Breaks when:* the store path builds `delta` from the rows rather than from
  `parse` on `content`.

- **INV-5 — The export equals the store.** After every successful write, the
  file's bytes equal `content`, their hash is `export_sha256`, and the mode is
  0444.
  *Test:* the bundle reads the file back after each op.
  *Breaks when:* the file is written before the edit, or the hash is taken of
  the pre-edit text.

- **INV-6 — Drift refuses and changes nothing.** With the export edited by
  hand, every write op refuses `export_drift`, and the file and the rows are
  byte-identical before and after.
  *Test:* the bundle edits the exported file, runs each write op, and
  compares.
  *Breaks when:* the drift check runs after the rows are rewritten, or a
  write overwrites the hand edit.

- **INV-7 — Concurrent appends both land.** Two `append_finding` calls on one
  reporter from two connections each leave their finding in `content` and in
  the file.
  *Test:* the bundle runs two appends from two threads, each on its own store
  connection, released together.
  *Breaks when:* the reporter row is read before `BEGIN IMMEDIATE`, so both
  writers edit the same old text.

- **INV-8 — A file is imported once.** Resolving the same `export_path` twice
  creates one row. Two first touches at once create one row, and both calls
  succeed.
  *Test:* the bundle resolves a fresh file twice, then from two threads, and
  counts rows.
  *Breaks when:* the import runs outside the write transaction, so the second
  insert hits `UNIQUE(export_path)` and the call fails.

- **INV-9 — A first filing registers its reporter.** `append_finding` for a
  path with no file and no row creates the row and the file. The file's bytes
  equal what today's create path writes for the same call.
  *Test:* the bundle compares against a golden file, committed before the
  store change and made by today's create path.
  *Breaks when:* the create path skips the skeleton or the header block.

- **INV-10 — Retired ops write nothing.** Each op in § 4.6's retired row
  refuses `retired`, and the file and the store are unchanged.
  *Test:* the bundle calls each one and compares.
  *Breaks when:* a retired op is left reachable through its old handler.

- **INV-11 — A fresh store and an upgraded one match.** A store created at
  version 5 and one climbed from 4 hold identical schema text.
  *Test:* the existing ANTS-3781 INV-8 test in
  `tests/features/roadmap_store_upgrade/`, which compares the two stores'
  schema. It reads the table list from `sqlite_master`, so it covers the new
  tables with no change.
  *Breaks when:* the rung carries its own copy of the DDL that drifts from
  `createSchema()`'s.

- **INV-12 — `feedback_pending` counts stay the same.** For a corpus whose
  files all import, the block's `files_with_pending`, `total_pending_lines`, `total_awaiting`
  and `total_suspected_untagged` equal those computed by `FeedbackFile::parse`
  over the same files.
  *Test:* `feedback_query_shared_root`, extended to compute both on its
  fixture root and compare.
  *Breaks when:* the store count includes `closed` findings, or skips a
  reporter whose row was imported during the same call.

- **INV-13 — Path validation is unchanged.** Every refusal
  `resolveFeedbackPath` gives today it still gives, and no store row is
  created for a refused path. This is the boundary between a caller's
  `path` and the filesystem.
  *Test:* the existing refusal cases in `mcp_feedback_log` and
  `mcp_feedback_query`, plus a row count after each.
  *Breaks when:* the reporter lookup or import runs before path resolution.

## 6. Failure modes

- **The disk fills during the export write.** `QSaveFile` fails, the
  transaction rolls back, and the verb refuses `write_failed`. The old file
  and the old rows stand.
- **The commit fails after the file was written.** The file holds text the
  store does not. The next write sees drift and refuses; `reimport` recovers
  it. Rare: a commit after the work is done fails only on I/O.
- **The corpus root moves** (`claude.mcp_feedback_root` changes). The new
  paths have no rows, so their files are imported fresh. The old rows stay
  with their old paths and no verb reaches them. They are not lost; cleaning
  them up is out of scope (§ 9).
- **A file is deleted.** The next write recreates it from the store. A read
  answers from the store. A call whose path is derived from `caller_cwd` may
  resolve a different name once the file is gone (`resolveFeedbackPath`'s
  sibling-stem adoption, ANTS-3439). That name has no row, so it starts a new
  reporter.
- **A file is not valid UTF-8.** The import refuses `not_utf8` and creates
  no row. The file is left as it is.
- **The terminal binary is older than the store.** Its Roadmap window fails
  until relaunch (§ 4.8).

## 7. Tests

One new bundle, `tests/features/feedback_store/`, labelled `features`, holds
INV-1, INV-2, INV-3, INV-4, INV-5, INV-6, INV-7, INV-8, INV-9 and INV-10. It opens a temporary store and a temporary corpus root, never the
live ones. The fixtures are copies of corpus files trimmed to the
cases INV-1 and INV-2 name.

INV-11 is the existing ANTS-3781 INV-8 test in
`tests/features/roadmap_store_upgrade/`. INV-12 extends
`feedback_query_shared_root`. INV-13 extends `mcp_feedback_log` and
`mcp_feedback_query`.

The existing feedback bundles for retired ops (`feedback_log_compact_shipped`,
`feedback_log_prune_tracking`, `feedback_log_compact_resolved`,
`feedback_log_migrate_v2`) lose their op-level cases. Their pure text
functions are unused after this, so the cases go with them. Of the
`FeedbackShipDate` cases in that bundle, the compaction ones go
(`StubCarriesMaxDateAndDatelessFallback`, `DatedStubIsIdempotent`,
`LiveCompactStampsShipDate`). The rest stay, because `feedback_query` still
calls `shipDateFromRoadmapBody` (ANTS-3504). In `mcp_feedback_log`, every `append_tracking` and
`set_format_version` case now expects `retired`.

Must fail first: every invariant in the new bundle fails on today's build,
which has no store path. INV-11 fails until rung 5 exists. INV-12 and INV-13
pass on both, and guard against a regression.

## 8. Alternatives considered (and rejected)

- **A column per field** (What, Repro, Impact, Suggested fix, Proposed ID,
  Note), with the file rendered from them. Rejected: the files are not
  regular enough to rebuild. The Proposed ID line sits at varying depths
  within a finding (an `awk` over the corpus measuring lines from `###` to
  `Proposed ID` finds offsets of 2, 4, 5, 6, 8 and more). Prose sits outside
  findings in 16 of the 30 files (`awk`, counting non-blank, non-heading,
  non-quote lines outside a `###` block, 2026-11-04). A renderer that reproduced them
  would be a second parser to keep in step. Storing the text and deriving
  the fields buys the same query.
- **A `project` row per reporter.** Rejected: two reporters have no roadmap,
  and adding roadmap-less rows to `project` reaches every consumer that walks
  `project` (`session_message` broadcast, `roadmap_query` scope `all`).
- **Keep the files primary and add a lock.** Rejected: it fixes § 2 item 1
  only, and the user chose the store (§ 3).
- **A one-shot bulk migration op.** Rejected: import on first touch needs no
  ordering step, and covers a file written by hand on another machine. The
  `feedback_pending` scan imports the whole corpus on the maintainer's next
  orient anyway.
- **Keep `compact_resolved`.** Rejected: it exists to keep files small, and
  the file is now an export nobody edits.

## 9. Out of scope

- Cross-reporter query ops beyond `feedback_pending` (by reporter, time to
  ship) — deferred; not yet queued.
- History of `content` revisions — deferred; not yet queued.
- Cleaning up rows whose `export_path` no longer resolves — deferred; not
  yet queued.
- Linking findings to roadmap items through `feedback_ref` — deferred; not
  yet queued. That table and its export record (ANTS-3761) stay as they are.

## 10. What checks this

| Rule | What catches a breach |
|------|----------------------|
| INV-1 | `tests/features/feedback_store/` |
| INV-2 | `tests/features/feedback_store/` |
| INV-3 | `tests/features/feedback_store/` |
| INV-4 | `tests/features/feedback_store/` |
| INV-5 | `tests/features/feedback_store/` |
| INV-6 | `tests/features/feedback_store/` |
| INV-7 | `tests/features/feedback_store/` |
| INV-8 | `tests/features/feedback_store/` |
| INV-9 | `tests/features/feedback_store/` |
| INV-10 | `tests/features/feedback_store/` |
| INV-11 | `tests/features/roadmap_store_upgrade/` |
| INV-12 | `tests/features/feedback_query_shared_root/` |
| INV-13 | `tests/features/mcp_feedback_log/`, `tests/features/mcp_feedback_query/` |
| INV-1 over the live corpus | **Partial:** the bundle runs on fixtures. A live file with a shape the fixtures miss is caught only by its own first import, which writes nothing on mismatch |
| § 4.8's relaunch note | **nothing** — a statement for whoever ships it |

## 11. Cross-doc impact

- `docs/standards/mcp-feedback-files.md`: the file is an export of the store;
  `reimport`; the retired ops leave § Maintainer compaction and § v1 legacy
  compaction ops.
- ANTS-3421, ANTS-3442, ANTS-3443 and ANTS-3446: each status line says its op
  was retired by ANTS-4513.
- ANTS-1962 and ANTS-3447: a note that the write now commits to the store
  first; their invariants are otherwise unchanged.
- `docs/standards/mcp-behavioural-notes.md`: the `feedback_log` and
  `feedback_query` entries.
- The `feedback_log` tool description: drop the retired ops, add `reimport`.
- CHANGELOG under `[Unreleased]`.

## 12. Cold-eyes loop log

Rows live in `../reviews/ANTS-4513-feedback-corpus-store-loop-log.md`.

## 13. Resource cost

The store grows by the corpus size, 854,071 bytes today (§ 2), plus one row
per finding. Each write rewrites one reporter's `content`; the largest file
is 154,949 bytes (`stat -c %s` over the corpus, 2026-11-04). No new
dependency.

## 14. Migration / compatibility

There is no migration step: § 4.3 imports each file on first touch. Callers
keep their arguments. Observable changes: the six retired ops now refuse; an
edited export refuses writes until `reimport`; a file that fails import
(`not_utf8`, `not_v2`) is refused by both verbs and listed under
`feedback_pending.import_refused`.
