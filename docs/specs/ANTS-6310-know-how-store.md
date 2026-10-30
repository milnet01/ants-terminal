# ANTS-6310 — a machine-global know-how store and its `know_how` verb

**Status:** shipped (2026-10-30).
**Kind:** feature.
**Source:** ROADMAP ANTS-6310 (claude-config session request, 2026-10-30).

**Layman:** One shared notebook where any Claude session writes down how to reach a machine or use a tool, so other projects look it up instead of working it out again.

## 1. Goal

A session in any registered project can record a lasting practical fact once,
under a stable topic. A session in any project can then find that fact in about
two small MCP calls. Sessions learn that the store exists without a noticeable
rise in startup cost.

## 2. Problem

Several projects need the same environment facts, such as how to reach the one
Windows machine. Each session works them out again. Nothing on this machine
holds such a fact where every session can write and find it:

1. `session_message` (`RemoteControl::cmdSessionMessage`) delivers mail to a
   project. It is not a place to look a fact up.
2. `session_memory` is keyed per cwd, and its writes must match the focused
   tab's cwd.
3. A file under `~/.claude` is behind `~/.claude/hooks/global-config-lock.sh`, so most
   project sessions cannot write it.

## 3. Scope decisions (agreed with the user)

All through the claude-config session, whose messages of 2026-10-30 report the
user's approval of the design. The user has not confirmed items 3 to 5 directly.

1. **What belongs.** How-to facts about machines, tools and environments useful
   to more than one project. Not chat, status or requests, not one project's
   internals, and never a credential. *(User, via claude-config.)*
2. **Two constraints rule everything.** Startup cost must not grow noticeably.
   Sessions find the one fact they need without reading the store whole.
   *(User, via claude-config.)*
3. **Its own file, `knowhow.sqlite`, not a table in `roadmap.sqlite`.** § 8 says
   why. *(Decided by claude-config on this session's recommendation.)*
4. **Any registered project writes, with no approval step.** *(User, via
   claude-config.)*
5. **`add` refuses an existing topic and `update` refuses a missing one**, and
   `update` can refuse when the entry changed since it was read. *(This session;
   agreed by claude-config.)*
6. **The hook line names the verb and shows no count, and the overwrite check
   compares a revision number rather than `last_verified`.** Both refine the
   agreed design; § 8 says why. *(This session; claude-config told.)*
7. **A `purge` op permanently erases a topic or one old version.** It is the one
   exception to entries never being deleted, so that a leaked secret can be removed
   without a hand `sqlite3` job. *(User, 2026-10-30, via claude-config. How it is
   guarded is this session's call, which the ruling left open.)*

## 4. Surface

### 4.1 The store

`KnowHowStore` (new: `src/knowhowstore.h`, `src/knowhowstore.cpp`, in the same
library as `RoadmapStore`) owns the file
`QStandardPaths::GenericDataLocation + /ants-terminal/knowhow.sqlite`. The test
bundle's `XDG_DATA_HOME` sandbox therefore covers it. It holds no connection
between calls: the handler constructs it per call, as `cmdSessionMessage` does
with `RoadmapStore`. `KnowHowStore` holds every op, its refusals and its reply
objects, so `test_core` can drive them. `RemoteControl::cmdKnowHow` resolves the
caller, supplies `author` and dispatches.

```sql
CREATE TABLE know_how (
    topic         TEXT PRIMARY KEY,
    title         TEXT NOT NULL,
    body          TEXT NOT NULL,
    tags          TEXT NOT NULL,   -- ' windows ssh ': space-delimited, both ends
    author        TEXT NOT NULL,   -- the writing project's export_slug
    created       TEXT NOT NULL,   -- UTC, yyyy-MM-ddTHH:mm:ssZ
    last_verified TEXT NOT NULL,   -- same form
    revision      INTEGER NOT NULL,
    retired_at    TEXT             -- NULL while active
);
CREATE TABLE know_how_history (
    topic       TEXT NOT NULL REFERENCES know_how(topic),
    revision    INTEGER NOT NULL,  -- the revision this row held
    title       TEXT NOT NULL,
    body        TEXT NOT NULL,
    tags        TEXT NOT NULL,
    author      TEXT NOT NULL,
    replaced_at TEXT NOT NULL,
    PRIMARY KEY (topic, revision)
);
CREATE TABLE know_how_purge_log (
    purged_at TEXT NOT NULL,
    topic     TEXT NOT NULL,
    revision  INTEGER,             -- NULL when the whole topic went
    author    TEXT NOT NULL,       -- the purging project's export_slug
    reason    TEXT NOT NULL
);
```

Every connection sets `PRAGMA secure_delete = ON`, because an `update` moves a body
that `purge` may later need to erase, and text freed without it stays in the file.

`PRAGMA user_version` starts at 1. A newer file is refused, as
`RoadmapStore::createSchema` refuses one. The file and both WAL sidecars are
owner-only, through `setOwnerOnlyPerms` (`src/secureio.h`),
in the order `RoadmapStore::open` uses: the empty file is secured before SQLite
writes to it. A write that cannot secure them refuses `io_error` and removes a file it
created, because entries are text sessions wrote and that header's policy refuses for
such a file. The roadmap store's warn half rests on locking every project out, which
does not apply here. A read warns through `warnNotOwnerOnly` and still answers.

**Only a committed row creates the file.** With no file, `find` and `list` answer
empty and `get` refuses `not_found`. A write that commits no row, whether a refusal
or a `dry_run`, leaves an absent file absent. So the file exists exactly when some session has
written, which § 4.4's hook relies on.

### 4.2 The verb

`know_how`, with `caller_cwd` required on every op and `op` defaulting to `find`.

| op | Arguments | Reply |
|---|---|---|
| `find` | `query` and/or `tag`; `limit` (default 5, max 20) | `results[]` of `{topic, title, snippet, age_days, stale}`; `count`, the results returned; `searched`, the active entries scanned |
| `get` | `topic`; `history` (bool) | `{topic, title, body, tags, author, created, last_verified, revision, age_days, stale, retired}`; with `history:true`, `history[]` of its older rows |
| `list` | `tag`; `limit` (default 50, max 200); `offset` | `topics[]` of `{topic, title, stale}`; `total`; `next_offset` when more remain |
| `add` | `topic`, `title`, `body`; `tags[]`; `dry_run` | `{topic, revision}` |
| `update` | `topic`; any of `title`, `body`, `tags[]`; `expected_revision`; `dry_run` | `{topic, revision, last_verified}` |
| `retire` | `topic`; `dry_run` | `{topic, retired_at}` |
| `purge` | `topic`, `expected_revision`, `reason`; `revision`; `dry_run` | `{topic, revision, rows_erased, file_scrubbed}` |

Refusals: the existing `bad_args`, `missing_field`, `no_project`, `io_error` and
`not_found`, and four new codes: `topic_exists`, `conflict`, `secret_detected` and
`store_full`.

`tags` is an array in every reply, `history[]` rows included.

**`find`.** The query splits on whitespace, tokens past the eighth are ignored,
and a token shorter than 2 characters is dropped. An entry's score sums, over its tokens, 3
for a match in `topic`, `title` or `tags`, or else 1 for a match in `body`.
Entries scoring above zero come back highest score first, then most recently
verified. Any-token matching is what lets a sentence work: "how do I reach the
Windows box" finds `windows-box` although no entry holds every word. Matching is
SQL `LIKE` with `%`, `_` and the escape character escaped, so it folds case for
ASCII only. A `tag` must match one tag whole, and filters the entries. With no `query`,
active entries holding the tag come back up to `limit`, most recently verified first. A
`query` left with no token answers empty. Retired entries are left out, of `list`
and its `total` too. A `limit` above its maximum is clamped. The
snippet is the first body line holding the entry's first matched token, or the
first non-blank body line, clipped to 120 characters on a surrogate boundary.

**`update`.** With no `title`, `body` or `tags`, it records that the entry was
checked and is still true: `last_verified` moves and no field, `revision` or
history row changes. With a
real change, the old row goes to `know_how_history`, the fields change,
`revision` rises by one, `last_verified` moves and `author` becomes the caller.
Only the newest 20 history rows per topic are kept. `update` on a retired topic
makes it active again. `expected_revision`, when given and not current, refuses
`conflict` and changes nothing.

**`retire`** sets `retired_at` and raises `revision` by one, adding no history row,
so a writer who read before the retire refuses `conflict`. Retiring a retired entry
changes nothing. The row stays, `get` still reads it with
`retired:true`, and `add` on that topic refuses `topic_exists` and says to use
`update`. An accidental overwrite or retire can be recovered, because only `purge` and
the 20-row history trim delete a row.

**`purge`** erases one history version when `revision` names it, and otherwise the
topic with all its history. `revision` equal to the current one refuses `bad_args`,
and the refusal says to `update` the text first and then purge that version.
`expected_revision` and `reason` are required, and their absence refuses
`missing_field`. A stale `expected_revision` refuses `conflict`, and a `revision`
with no history row refuses `not_found`. So nobody erases what they have not just
read. `reason` is at most 200 bytes and goes through the secret check. Each purge
appends one `know_how_purge_log` row, which holds no title, body or tags. Only the newest 1000
log rows are kept. No op reads that table; it is there for a person asking what went.

After the commit, `purge` runs `PRAGMA wal_checkpoint(TRUNCATE)`. `file_scrubbed` is
true when that reports not busy. It is false while another connection is inside a
transaction on the file. The text then stays on disk until the last connection to the
file closes, and the reply says so.
The op erases the live file only. A backup, or a block the disk's own firmware has
remapped, is out of its reach, and the verb's description says so. A secret in a
`topic` name stays in the purge log. The secret check refuses the known shapes before
such a name can exist, and the log row can be removed by hand.

**Staleness.** `age_days` counts whole days since `last_verified`. `stale` is
true when `age_days` is over 90. Time comes through a clock the test can set.

**Limits** (a breach refuses `bad_args`, except the last):

| Field | Limit |
|---|---|
| `topic` | `^[a-z0-9-]+$`, at most 64 bytes |
| `title` | non-empty, at most 120 bytes, no newline |
| `body` | non-empty, at most 16 KiB |
| `tags` | at most 8, each `^[a-z0-9-]+$` and at most 32 bytes |
| topics, retired ones included | at most 1000; `add` beyond it refuses `store_full` |

### 4.3 Who may do what

Reads need only a `caller_cwd` that resolves (`ants::requiredCallerCwd`); the
facts are machine-wide, and reads open no roadmap store. A write also opens `RoadmapStore`, the ordinary way, to read the caller's project:
`projectIdContaining(ProjectSettings::repoRootOf(cwd))` resolves the caller, as `cmdSessionMessage` does, and `readProject` supplies its
`exportSlug` for `author`. An unregistered caller refuses `no_project`, and so does
a deregistered one, for which `readProject` returns nothing. The slug
is stored as text because a project id is no foreign key across files. The verb
writes nothing to `roadmap.sqlite` itself. `RoadmapStore::open` may still create
or climb that file, as it does for every verb, and `~RoadmapStore`'s
`PRAGMA optimize` may add SQLite's own statistics table.

**Secrets.** `add` and `update` run `SecretRedact::scrub` (`src/secretredact.h`)
over `topic`, `title`, `body` and the tags. A non-zero `redactedCount` refuses
`secret_detected` and writes nothing. The refusal names the count and never
echoes the matched text. The verb's description says the check catches known
secret shapes only, and that an entry says how to authenticate, never the secret.

### 4.4 Discovery

**The hook.** New `hooks/ants-know-how-notice.sh`, added under `SessionStart` to
`hook_entries` and to `pack_hook_regex` in `tools/install-hooks.sh`. It is not gated on `.ants-project`, because the
store serves every project. It stats
`${XDG_DATA_HOME:-$HOME/.local/share}/ants-terminal/knowhow.sqlite` and, if
present, prints one line, and otherwise nothing:

```
[ants:know-how] shared how-to facts for machines and tools: know_how op:"find"
```

It spawns no `ants-mcpd` and no `sqlite3`.

**The prelude.** `kOrientationScriptBody` (`src/mcporientation.cpp`) gains
`Shared how-to facts: know_how.` That is all the room its 500-byte cap leaves
(`mcp_orientation_install`'s INV-10). The MCP `initialize` reply has no
`instructions` field to use instead.

`session_orient` gains nothing. The hook and the prelude already reach a session
before it would call `session_orient`.

## 5. Invariants

The clauses below name a test file that does not exist yet, so each states the
rule its fixture isolates. All are in
`tests/features/know_how/test_know_how.cpp` unless a clause says otherwise.

- **INV-1** — Only a committed row creates the store. `find`, `list` and `get` against a
  path with no file answer empty with `searched` 0, empty and `not_found`, and the
  file is still absent afterwards. So is it after a `dry_run` `add` and an `update`
  refusing `not_found`.
  *Test:* `tests/features/know_how` `KnowHow.ReadsDoNotCreateTheFile`.
  *Breaks when:* the constructor, a read op or a write that commits nothing calls
  the code that creates the file, so a session that wrote no entry would switch
  the hook line on.

- **INV-2** — One entry per topic. `add` on an existing topic refuses
  `topic_exists` and leaves the row unchanged, retired rows included. `update`
  and `retire` on a missing topic refuse `not_found`.
  *Test:* `tests/features/know_how` `KnowHow.AddAndUpdateRefuseTheWrongState`, whose second `add` carries a
  different body so that an upsert would be visible.
  *Breaks when:* `add` is an upsert, or checks active rows only.

- **INV-3** — A change keeps what it replaced. An `update` that changes a field
  moves the old row to history, raises `revision` by one and moves
  `last_verified`. An `update` with no field moves `last_verified`, leaves
  `revision` alone and adds no history row. On a retired topic, either form makes
  it active again. Only the newest 20 history rows per topic survive.
  *Test:* `tests/features/know_how` `KnowHow.UpdateKeepsHistory`, with 22 changes under a clock set forward
  at each step, then one fieldless `update`, then a `retire` and a fieldless
  `update`.
  *Breaks when:* the old body is overwritten without a history row, the fieldless
  form writes one, or the trim keeps the oldest rows.

- **INV-4** — A stale writer cannot overwrite. `expected_revision` not equal to
  the current revision refuses `conflict` and changes nothing, including when a
  `retire` is what moved it.
  *Test:* `tests/features/know_how` `KnowHow.ExpectedRevisionRefusesAStaleWrite`.
  *Breaks when:* the check is skipped, or it runs after the write.

- **INV-5** — A secret-shaped value is never stored or echoed. A body or title
  holding a GitHub-PAT-shaped string, or a tag holding a `glpat-` string, each
  built at run time so that no secret scanner flags the test source, refuses
  `secret_detected` on both `add` and `update`. So does an `add` whose topic is a
  `glpat-` string. No row or
  history row changes, and the reply does not contain the value. A body saying
  "SSH key ~/.ssh/win_ed25519" is accepted.
  *Test:* `tests/features/know_how` `KnowHow.SecretShapesAreRefused`.
  *Breaks when:* only `add` is checked, only `body` is checked, or the refusal
  quotes the match.

- **INV-6** — `find` answers small, and answers a sentence. With an entry
  `windows-box` and a second entry whose body alone holds the words "how", "do",
  "reach" and "the", the query "how do I reach the Windows box" returns
  `windows-box` first, although the second entry matches more tokens. No result
  carries a `body` key, every snippet is at most 120 characters, a retired entry
  is absent from `find` and from `list` and its `total`, and `limit` is honoured.
  `searched` counts the active entries. A `tag` alone returns the active entries
  holding it. A query of `50%` matches an entry holding `50%` and not one holding
  `500`.
  *Test:* `tests/features/know_how` `KnowHow.FindRanksAndStaysSmall`.
  *Breaks when:* tokens are ANDed (no result), scores are unweighted, the body
  leaks into a row, or `LIKE` wildcards go unescaped.

- **INV-7** — Only a registered project writes, and the author is its slug. A
  write from an unregistered `caller_cwd` refuses `no_project` and creates no
  `knowhow.sqlite`. A read from the same place succeeds. A write from a deregistered project refuses
  `no_project`. A write from a registered project's
  subdirectory records that project's `export_slug` as `author`.
  *Test:* `tests/features/know_how` `test_know_how_verb.cpp`, driving
  `RemoteControl::cmdKnowHow` in `test_claude`, as `project_repo_root` does for
  `cmdSessionMessage`.
  *Breaks when:* the handler trusts a caller-supplied author, or calls
  `registerProject()`, which would register any root it is handed.

- **INV-8** — The roadmap store is untouched. After a write through the handler,
  `roadmap.sqlite`'s `user_version` is as it was, and so are its `sqlite_master`
  rows, leaving out SQLite's own `sqlite_stat*` tables.
  *Test:* `tests/features/know_how` `KnowHowVerb.WritesLeaveTheRoadmapStoreAlone`.
  *Breaks when:* the store opens the wrong path, or a table lands in the roadmap
  schema, which ANTS-3781's created-versus-climbed comparison
  forbids at an unchanged version.

- **INV-9** — Staleness flips after 90 days, not at them. An entry verified
  exactly 90 days ago has `stale:false`, and at 91 days `stale:true`, on `find`,
  `get` and `list` alike.
  *Test:* `tests/features/know_how` `KnowHow.StaleAfterNinetyDays`, using the settable clock.
  *Breaks when:* the comparison is `>=`, or one op computes it separately.

- **INV-10** — The file is owner-only and a newer one is refused. After a write,
  with a second connection holding the file open so that the sidecars still exist,
  the file and both sidecars are mode 0600. A file with a `user_version` above the
  build's refuses `io_error` and is left unchanged.
  *Test:* `tests/features/know_how` `KnowHow.FileModeAndNewerSchema`.
  *Breaks when:* a file or sidecar keeps wider permissions, or a newer schema is
  opened and written anyway.

- **INV-11** — Limits refuse rather than truncate. Each limit in § 4.2's table accepts its
  maximum, and refuses one over it without a write. `add` of the 1001st topic
  refuses `store_full`, whether or not earlier topics are retired.
  *Test:* `tests/features/know_how` `KnowHow.LimitsRefuse`.
  *Breaks when:* a value is clipped silently, bytes are counted as characters, or
  retired rows escape the cap.

- **INV-12** — Discovery costs one line, or nothing. The hook prints exactly the
  § 4.4 line with the store file present, under `XDG_DATA_HOME` set and unset, and
  nothing with it absent, in a directory with no `.ants-project`. Uninstall removes
  its settings entry. Its source names neither `ants-mcpd` nor
  `sqlite3`. The prelude holds `know_how` and stays within its 500-byte cap.
  *Test:* `tests/features/hook_pack` for the hook;
  `tests/features/mcp_orientation_install`'s INV-10 and a new text case for the prelude.
  *Breaks when:* the hook gates on `.ants-project`, spawns a process, or a later
  prelude line pushes the cap.

- **INV-13** — `purge` erases the text from the file, and only what it names. After
  an `add` with a marker in its body, an `update` replacing it and a `purge` of the
  old revision, with every connection closed, the marker's bytes are in neither the
  file nor a sidecar, and the current body is intact. With a second connection inside a
  read transaction, the reply says `file_scrubbed:false`. With a second connection open
  but idle, it says `file_scrubbed:true` and the marker is gone while that connection
  stays open. A `purge` of the whole topic removes
  the row and its history, and frees its place under the topic cap. Without
  `expected_revision` or `reason` it refuses `missing_field`, with a stale
  `expected_revision` it refuses `conflict`, and on the current revision it refuses
  `bad_args`, each changing nothing. The log row holds the reason and no marker.
  A `reason` holding a secret shape refuses `secret_detected`.
  *Test:* `tests/features/know_how` `KnowHow.PurgeErasesFromTheFile`, reading the
  files' raw bytes, on a body long enough to need its own overflow pages. Closing the
  last connection also checkpoints, so only the idle-connection case fails when the
  checkpoint is removed. A control repeats the sequence on the test's own connections
  with `secure_delete` OFF and sees the marker, which proves the read can fail. This
  machine's SQLite is built with `SECURE_DELETE` (`PRAGMA compile_options`, 2026-10-30),
  so removing the pragma leaves the bytes clean here, and the test also checks the source
  of `KnowHowStore`'s connection setup for it.
  *Breaks when:* the pragma leaves the connection setup, the checkpoint
  is skipped, a busy checkpoint reports `file_scrubbed:true`, or the log keeps the body.

## 6. Failure modes

- **The file is locked by another writer.** The `RoadmapStore` Interactive
  deadline applies (`kBusyTimeoutMs`), and then `io_error`. Nothing is half-written:
  each write is one transaction.
- **The file is corrupt or unreadable.** `io_error` naming the path. The verb does
  not recreate it, which would silently discard the entries. The hook still prints,
  since it only stats the file, and the session then sees the refusal.
- **The roadmap store cannot open during a write.** `io_error`, and nothing is
  written, because the author is unknown.
- **The hook's file test races a first write.** A session misses the line once and
  gets it next time. Harmless.
- **A false positive from the secret check.** The writer rephrases. There is no
  override flag, since a flag is what a hurried session would reach for.
- **A real secret of an unknown shape.** Stored. The description warns of this, and
  `purge` removes it once someone notices.
- **Disk loss.** The file has no other copy. See § 9.

## 7. Tests

`tests/features/know_how/`, label `features;fast`, with `spec.md` pointing here.

| Test | Bundle | Locks |
|---|---|---|
| `test_know_how.cpp` | `test_core` | INV-1, INV-2, INV-3, INV-4, INV-5, INV-6, INV-9, INV-10, INV-11, INV-13 |
| `test_know_how_verb.cpp` | `test_claude` | INV-7, INV-8 |
| `tests/features/hook_pack`, `tests/features/mcp_orientation_install` | their own | INV-12 |

`tools/check-readme-claims.sh` holds the verb count. No check common to every verb
holds the declared-input rule, so `KnowHowVerb.SchemaDeclaresEveryArgumentRead`
holds it for this verb.

Each new test is seen to fail before its rule exists, and `ctest -N -R KnowHow` is
checked to have moved after building. `ants_add_core_bundle` and
`build_target_for` name the bundle.

## 8. Alternatives considered (and rejected)

- **A table in `roadmap.sqlite`, no version bump.** Breaks the store's rule that a
  version means one set of tables, whether created or climbed (ANTS-3781 INV-8).
- **A table in `roadmap.sqlite`, version bump.** The first new binary to open the
  store climbs it, and every older binary still running then refuses the store for
  every project (`RoadmapStore::createSchema`'s newer-schema arm), so an old
  terminal loses its roadmap until it relaunches. The know-how store is not worth
  that, now or on any later change to it.
- **Extending `session_memory`.** Per-cwd JSON, and writes are gated on the focused
  tab.
- **A markdown file under `~/.claude`.** Behind the config lock, and it needs a
  whole-file read to search, which constraint 2 rules out.
- **SQLite FTS5 for `find`.** Depends on how the system SQLite was built, for a
  store capped small enough that `LIKE` serves. `LIKE`'s ASCII-only case folding is
  the accepted cost.
- **Requiring every token to match.** A session asking in a sentence gets nothing.
- **A topic count in the hook line, read through `ants-mcpd`.** Adds a process to
  every session start, which constraint 1 is against. `ants-inbox-notice.sh` pays
  that cost and bounds it with `timeout 2`.
- **A topic count read from a small summary file.** A second copy of a fact, which
  can drift from the store, to tell a session something `list` tells it for free
  when it cares.
- **Topic names in the hook line.** Grows with the store, on every session.
- **Comparing `last_verified` for the overwrite check.** It has one-second
  resolution, so two writes in one second would pass it. A revision number is exact.
- **A `session_orient` field.** A second copy, in a reply every session already pays
  for, of what the hook and prelude say.

## 9. Out of scope

- Backing up `knowhow.sqlite`. Losing this file costs rediscovery, not work — shipped
  separately as ANTS-6311.
- A UI for browsing the store. A permanent exclusion for now: the readers are sessions, and they have the verb.
- The `CLAUDE.md` line telling sessions to check the store. Owned by claude-config,
  which adds it once this ships.

## 10. What checks this

| Rule | What catches a breach |
|------|----------------------|
| INV-1 | `KnowHow.ReadsDoNotCreateTheFile` |
| INV-2 | `KnowHow.AddAndUpdateRefuseTheWrongState` |
| INV-3 | `KnowHow.UpdateKeepsHistory` |
| INV-4 | `KnowHow.ExpectedRevisionRefusesAStaleWrite` |
| INV-5 | Partial: `KnowHow.SecretShapesAreRefused` covers the known shapes; **nothing** catches a secret of an unknown shape, which is why § 4.3 says so in the verb's description |
| INV-6 | `KnowHow.FindRanksAndStaysSmall` |
| INV-7 | `test_know_how_verb.cpp` |
| INV-8 | `KnowHowVerb.WritesLeaveTheRoadmapStoreAlone` |
| INV-9 | `KnowHow.StaleAfterNinetyDays` |
| INV-10 | Partial: `KnowHow.FileModeAndNewerSchema` sees the final modes; **nothing** observes § 4.1's order, the empty file secured before SQLite writes to it, or forces its `chmod` refusal |
| INV-11 | `KnowHow.LimitsRefuse` |
| INV-12 | `hook_pack` and `mcp_orientation_install` |
| INV-13 | Partial: `KnowHow.PurgeErasesFromTheFile` covers the live file and its sidecars; on a SQLite built with `SECURE_DELETE`, only its source check sees the pragma go; **nothing** reaches a backup or the disk's own remapped blocks, which is why § 4.2 says so in the verb's description |
| What belongs in an entry (§ 3 item 1) | **nothing** — no check can tell a how-to fact from chat; the verb's description and claude-config's `CLAUDE.md` line carry it |
| Discovery actually gets used (Goal) | **nothing** — no measure yet; reading the store's topics after some weeks of use is the check |

## 11. Cross-doc impact

- `README.md`: the verb count, through `tools/check-readme-claims.sh`.
- `docs/subsystems.md`: the new files.
- `docs/standards/mcp-behavioural-notes.md`: a `know_how` note.
- `docs/standards/mcp-error-codes.md`: the four new codes.
- `CHANGELOG.md`: under `[Unreleased]`.
- `~/.claude/CLAUDE.md`: claude-config's, once shipped. This session messages it then.
- `tools/install-hooks.sh` writes `~/.claude/settings.json`, so this session does not
  run it. claude-config, or the user, runs it once the hook ships.

## 12. Reload

The verb holds no running state, so it has no stop/start control: each call opens
and closes the file. Its code runs in `ants-mcpd`, which is what lets a new build
reach clients without the terminal. It is registered in `mcp::registerProjectScopedVerbs()` with
`CallerCwdContract::Required`, its body reaches no `MainWindow`, and `ants-mcpd`
serves it. It reaches clients on their next reconnect after `ants-mcpd` is
rebuilt, with no terminal relaunch.

The hook reaches new sessions once `tools/install-hooks.sh` has run. The prelude
sentence reaches them after the terminal next relaunches, because
`MainWindow::setupClaudeMcpProviders` rewrites that script. Nobody needs to
relaunch for this: until then, the hook line does the job.

## 13. Cold-eyes loop log

Rows live in `../reviews/ANTS-6310-know-how-store-loop-log.md`.

## 14. RAM / build cost

The caps bound the file: 1000 topics, each with a 16 KiB body and 20 history rows,
is about 340 MiB at the very worst. A real store is expected to stay far below
that, and a full store refuses rather than dropping anything. The purge log keeps
1000 rows of at most 200-byte reasons. Nothing stays in
memory between calls: no connection, cache or thread is held. No new build target,
and no new dependency: QSQLITE is already linked.

## 15. Migration / compatibility

None. A new file, a new verb and a new hook, and nothing reads them yet. A build
older than this one ignores the file. `kSchemaVersion` does not move.
