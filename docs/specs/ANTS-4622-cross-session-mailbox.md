# ANTS-4622 — Cross-session mailbox: a `message` table and its verb

**Status:** spec draft (2026-08-22).
**Kind:** feature.
**Source:** ROADMAP.md ANTS-4622 (user-request-2026-08-22).
**Composes with:** ANTS-4617 (`roadmap_migrate op:"deregister"` — the delete
order this table joins), ANTS-3781 (the schema-upgrade ladder), ANTS-4621 (the
schema-declares-what-the-handler-reads rule).

## 1. Problem

Claude Code sessions working on different projects cannot address each other.
Everything they share today is **broadcast and file-based**: the
`*_Ants_MCP_Feedback.md` corpus at `/mnt/Games/Scripts/Linux/`, parsed by
`FeedbackFile::parse()` and surfaced by the `feedback_pending` block in
`src/remotecontrol_state.cpp`. Twenty such files are scanned at session start
(`session_orient` → `feedback_pending.files_scanned`, measured 2026-08-22).

That channel has one shape and it is the wrong one for this. Three
consequences:

1. **It is addressed to one reader.** Every finding is addressed to the Ants
   maintainer, because its terminal state is an `ANTS-NNNN` roadmap id. A
   session on Vestige has nowhere to leave a note for whoever picks up RetroDB
   next.
2. **Nothing is ever acknowledged.** A contributor writes a finding and never
   learns whether anyone read it; status is derived from whether the maintainer
   filled a `**Proposed ID:**` slot, which is a statement about triage, not
   about receipt.
3. **It cannot be directed.** `feedback_pending` is gated on the reader
   shipping `docs/standards/mcp-feedback-files.md` — i.e. on being the Ants
   repo. No other project has an inbox of any kind.

### 1.1 Why this is not a second copy of the feedback corpus

The roadmap item requires this distinction to be stated crisply, and to build
neither channel if it cannot be. It can, and it is one line:

> **The feedback corpus asks for work to be filed. The mailbox asks for a
> person to know something.**

A feedback finding's terminal state is a **roadmap id**; a message's terminal
state is an **acknowledgement**. Neither converts into the other, and the two
are mechanically separable: a message is never triaged and never earns an
`ANTS-NNNN`; a feedback finding is never acked. Where a session wants work
filed, it writes a feedback finding. Where it wants a specific project's next
session to know a fact, it sends a message.

**The overlap that remains is deliberate and small**: both are asynchronous,
and both surface at session start. Sharing the delivery slot is what makes the
second channel cheap rather than duplicative.

## 2. Surface

### 2.1 Addressing — a project, never a session

**A CC session is not addressable.** `ClaudeIntegration` identifies a session
by shell PID and clears its per-session state on PID change
(`src/claudeintegration.cpp`, the `m_changedFiles.clear()` branch, ANTS-1168).
A PID is ephemeral, per-tab, and reused by the OS, so mail addressed to one is
undeliverable the moment that session ends — which is most of the time.

The store is keyed by **project**, and a project outlives every session that
touches it. So:

- `to` is an `export_slug`, resolved against `project.export_slug` (UNIQUE,
  already CHECK-constrained to `[a-z0-9][a-z0-9-]*`). Sixteen projects are
  registered as of 2026-08-22 (`sqlite3 -readonly … "SELECT count(*) FROM
  project;"`).
- An unresolvable slug **refuses** `unknown_project` and lists the known slugs.
  Mail that can never be delivered must not be accepted.
- The sending session is recorded as **provenance, not as a key**:
  `from_session` is free text a reader may use to reply in prose, and nothing
  routes on it.

**Self-addressed mail is allowed on purpose.** `from == to` is the note left
for the next session on the same project, which is the single most useful case
the item describes; a CHECK forbidding it would block it for symmetry's sake.

### 2.2 The `message` table, and the version 2 → 3 bump

`RoadmapStore::kSchemaVersion` is `2` (`src/roadmapstore.h:38`) and the live
store reads `PRAGMA user_version = 2`. This adds the tenth table and moves it
to `3`.

```sql
CREATE TABLE message (
  message_id      INTEGER PRIMARY KEY,
  from_project_id INTEGER NOT NULL REFERENCES project(project_id),
  to_project_id   INTEGER NOT NULL REFERENCES project(project_id),
  from_session    TEXT NOT NULL DEFAULT '',
  created_at      TEXT NOT NULL
                    CHECK (created_at GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z'),
  body            TEXT NOT NULL CHECK (length(body) > 0 AND length(body) <= 4096),
  acked_at        TEXT
                    CHECK (acked_at IS NULL OR acked_at GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z')
)
```

The `created_at` GLOB mirrors `history.changed_at` verbatim rather than
inventing a second timestamp convention.

**The DDL text is part of the schema.** `src/roadmapstore.cpp` states the rule
above its `ddl[]` array: SQLite stores a `CREATE TABLE` verbatim, comments
included, so a store built by `createSchema()` and one climbed by a rung agree
only if the two texts are byte-identical. A whole new table is the easy case —
unlike a column, a rung can issue the same `CREATE TABLE` statement the array
holds — **so the bump ships the identical string in both places, and the
rationale for it lives above the array rather than inside the SQL.**

The ladder takes exactly one rung per step and stamps `user_version` once after
the last (`src/roadmapstore.cpp`, the `rungs != 1` guard).

### 2.3 The verb: `session_message`, ops `send` / `inbox` / `ack`

One verb with ops, matching `roadmap_log` and `roadmap_migrate`, rather than
three thin verbs.

| op | Required | Optional | Returns |
|---|---|---|---|
| `send` | `caller_cwd`, `to`, `body` | `from_session`, `dry_run` | `message_id`, `to`, `created_at` |
| `inbox` | `caller_cwd` | `include_acked`, `limit`, `offset` | `messages[]`, `unacked_count` |
| `ack` | `caller_cwd`, `message_id` | — | `acked_at`, `already_acked` |

Refusal codes, per `docs/standards/mcp-error-codes.md`: `unknown_project`
(the `to` slug resolves to no row), `not_found` (`ack` on an id that does not
exist), `forbidden` (`ack` on a message addressed to another project),
`inbox_full` (§ 2.6), `bad_args` (empty or oversized body).

**Two contracts this verb inherits rather than re-derives:**

- **ANTS-4621 — the schema declares what the handler reads.** Every argument
  read via `req.value()` is declared in `inputSchema.properties`, and `op`'s
  enum carries every op. The schema sets `additionalProperties: false`, so an
  undeclared argument is refused by a strict client before the handler runs;
  and on the permissive path ANTS-2175 builds `ignored_args` from the same
  properties, so an undeclared argument is reported as ignored **on the call it
  just steered**. This shipped broken once, three days ago, on the neighbouring
  verb.
- **ANTS-4463 — a preview carries no past-tense field.** `send` with
  `dry_run:true` resolves the recipient and validates the body but writes no
  row, and reports `would_send` rather than `message_id`.

`ack` is **idempotent**: a second `ack` succeeds, does not move `acked_at`, and
returns `already_acked:true`. The first ack is the fact.

**Why two states and not three.** A third state — *acted on*, beyond *read* —
was considered and rejected. The v2 feedback format learned this the expensive
way: it dropped its stored tracking tables because status derived live beat
status stored, and a stored state nobody updates is worse than no state at all.
Two states answer the only question the sender actually has — *can I stop
wondering whether this was seen* — and what the recipient did about it is what
the reply body is for.

### 2.4 Delivery — `session_orient` carries the inbox

A mailbox nobody polls is a mailbox nobody reads. `session_orient` gains a
`mail_pending` block beside `feedback_pending`.

**It reports the INBOX and never the outbox.** ANTS-3631 records what the
other direction costs: `feedback_pending` originally counted awaiting markers,
which are the maintainer's *outbox*, so every session started with a
permanently non-zero to-do that only somebody else could clear. Mail a project
has **sent** and nobody has acked contributes zero to that project's own count.
For self-addressed mail the sender is the recipient, so it does appear — that
is delivery, not the outbox.

**Unlike `feedback_pending`, it is not gated on shipping a document.** That
gate exists because feedback triage is the Ants repo's job alone; an inbox
belongs to every registered project. A project with no mail emits no block, so
the envelope and its ETag are unchanged for the quiet case.

### 2.5 Deregistration — both ends, or the mail dangles

The schema declares `REFERENCES` and **no** `ON DELETE CASCADE` (measured:
`grep -c "ON DELETE CASCADE" src/roadmapstore.cpp` → 0), so every delete is
written out by hand. `RoadmapStore::deregisterProject()` walks nine tables in
foreign-key order — element, history, feedback_ref, relationship, citation,
item, section, id_prefix, project — and `message` becomes the tenth.

**It is cleared from both ends**, the way `relationship` already is: a message
*from* the deregistered project and one *to* it both reference a `project_id`
that is about to vanish, and either would dangle. `message` deletes before
`project`, and `DeregisterCounts` gains a `messages` field beside its existing
eight.

### 2.6 Retention — the table has no natural pruning event

Nothing in the design ever finishes with a message, so growth is unbounded
unless capped. Two rules, and they are deliberately asymmetric:

- **Acked mail is pruned after 30 days**, opportunistically on the next `send`
  by the same project. An acked message has done its job.
- **Unacked mail is never auto-pruned**, at any age. Deleting undelivered mail
  is the one prune that loses the thing the feature exists to carry; an old
  unread message means nobody has picked that project up, which is exactly when
  the message still matters.

Unbounded unacked mail is therefore bounded by a **cap instead of a clock**:
500 unacked messages per recipient. A `send` to a full inbox refuses
`inbox_full` rather than dropping the oldest, because a silently dropped
message is indistinguishable from one never sent.

## 3. Invariants

- **INV-1** — A message is addressed to a project. `send` resolves `to` against
  `project.export_slug` and refuses `unknown_project` when it does not resolve;
  no send path accepts a PID, tab id or session token as the recipient key.
  *Test:* `tests/features/session_message/` — send to a **well-formed but
  unregistered** slug returns `ok:false, code:"unknown_project"` and leaves
  `message` empty. The slug is syntactically valid, so the `export_slug` CHECK
  cannot be what rejects it; only the resolution step can.
- **INV-2** — `inbox` returns only messages whose `to_project_id` is the
  calling project, resolved from `caller_cwd`. *Test:* two registered projects,
  one unacked message each, sent seconds apart; each project's `inbox` returns
  exactly its own. Both are unacked and recent, so neither the ack filter nor
  retention can account for the exclusion — only the recipient predicate can.
- **INV-3** — Confirmation is two states held in one nullable column. The
  `message` DDL declares `acked_at` and no `status`, `state` or `read` column;
  unread is `acked_at IS NULL`. *Test:* schema assertion over `sqlite_master`,
  plus a second `ack` returning `already_acked:true` with `acked_at` unchanged.
- **INV-4** — `session_orient` reports the inbox, never the outbox. *Test:*
  project A sends to project B and nobody acks; **the pair is the assertion** —
  B's `mail_pending` counts one and A's is absent or zero. B's leg is what
  distinguishes a working direction predicate from a block that failed to render
  at all, which would also leave A at zero. The message is unacked, so an ack
  filter cannot produce A's zero either.
- **INV-5** — Deregistering a project removes its mail from both ends and no
  further. *Test:* A and B exchange messages and B also holds an unrelated
  message from C; deregister A; no `message` row referencing A survives, and
  B's message from C is untouched. The surviving row is what distinguishes a
  scoped delete from a table truncate.
- **INV-6** — The bump is expressed twice and the two agree: a store created at
  version 3 and one climbed from version 2 hold byte-identical `message` DDL in
  `sqlite_master`. *Test:*
  `tests/features/roadmap_store_upgrade/test_roadmap_store_upgrade.cpp`, the
  comparison ANTS-3815 INV-3 already performs, extended to the new table.
- **INV-7** — Every argument `session_message`'s handler reads via
  `req.value()` is declared in the verb's `inputSchema.properties`, and `op`'s
  enum carries `send`, `inbox` and `ack`. *Test:* the ANTS-4621 guard's shape —
  scrape the handler for its `req.value()` keys and assert each is declared,
  `caller_cwd` excepted as a universal dispatch arg (ANTS-2175 INV-2). Written
  against the source rather than the engine, because no test that drives the
  engine seam crosses the dispatcher.
- **INV-8** — Unacked mail survives the retention pass at any age. *Test:*
  an unacked message stamped older than § 2.6's TTL, then a `send` from the same
  project to trigger the prune; the old message remains. It is over the
  TTL, so an age filter alone would delete it — only the acked predicate spares
  it.
- **INV-9** — A full inbox refuses rather than drops. *Test:* fill a
  recipient to § 2.6's cap, send once more, assert `code:"inbox_full"` and that
  the recipient's row count is unchanged.
- **INV-10** — `dry_run` writes nothing and claims nothing in the past tense.
  *Test:* `send` with `dry_run:true` leaves `message` empty and its envelope
  carries `would_send` and no `message_id` (ANTS-4463).

## 4. RAM / build cost

**Store growth is capped by § 2.6, not by hope.** Worst case per recipient is
500 unacked × 4 KiB body = 1.95 MiB; across the 16 registered projects, 31.2 MiB
of `message` bodies before any `send` is refused (`python3 -c "print(500*4096*16/1048576)"`).
Acked mail adds at most 30 days of traffic on top and is pruned without operator
action.

**Resident cost is an inbox read, not the table.** `inbox` selects one
recipient's rows with a default `limit` of 20 and never loads the table;
`session_orient`'s block runs a `COUNT(*)`, which materialises nothing. The
`message_id` index is SQLite's implicit rowid primary key; one additional index
on `(to_project_id, acked_at)` serves both the inbox read and the count.

**No new build target and no new external library.** The table lives in
`src/roadmapstore.cpp`, the verb in a new
`src/remotecontrol_session_message.cpp` joining the existing `remotecontrol`
lane — which also keeps it clear of `src/remotecontrol_roadmap_log.cpp`, which
sits at exactly the 6000 lines ANTS-3833 INV-6 caps a TU at, with no headroom
(`wc -l`, 2026-08-22; ANTS-4620 tracks the split).

## 5. Out of scope

- **Replies as a first-class relation** — no `in_reply_to` column. A reply is a
  new message whose body quotes the one it answers. Threading is a shape that
  earns its cost only once a corpus exists to show what the threads look like;
  adding it now is inventing a contract from zero traffic. No id: this is a
  decision not to build it, not a deferral.
- **Notifying a live session.** Delivery is at session start, via
  `session_orient`. Interrupting a running session is a different feature with a
  different failure mode (an interruption nobody asked for), and the desktop
  notification rule already restricts what may fire unprompted.
- **Broadcast or group addressing.** `to` resolves to exactly one project. A
  session wanting every project's attention is writing a finding, which is what
  the feedback corpus is for — § 1.1.
- **Message editing or recall.** A sent message is immutable. Recall implies
  the sender can know it was not yet read, which the ack model deliberately
  does not promise.
- **Cross-machine delivery.** The store is machine-global, not network-shared.
  Everything here is one machine's sessions talking to each other.

## 6. Tests

Feature test: `tests/features/session_message/`, covering INV-1, INV-2, INV-3,
INV-4, INV-5, INV-8, INV-9 and INV-10 — written out rather than as a range, so
the parity between this list and § 3 stays mechanically checkable. Label
`features`. Wired into an existing bundle's `SOURCES`
list — ask `build_target_for` which bundle owns the file rather than guessing,
and check `ctest -N -R` moves before and after.

INV-6 extends `tests/features/roadmap_store_upgrade/`, the harness that already
compares a created store against a climbed one.

INV-7 is a source scrape, sited with the verb's own tests, following
`tests/features/roadmap_migrate_verb/`'s `Ants4621HandlerReadsOnlyDeclaredArgs`.

Every test is verified to fail against pre-implementation source before the
implementation is written, per the project's test-first rule. **Each one is
run twice — as written, and against a state that should break its
invariant** — since a clause that cannot tell pass from breach is a tautology
however well it reads.

## 7. Cross-doc impact

- `CLAUDE.md` — the roadmap-store paragraph names the nine-table delete order
  and its hand-written FK sequence; both become ten.
- `docs/standards/mcp-error-codes.md` — `unknown_project` and `inbox_full` if
  either is new to the taxonomy.
- `docs/standards/mcp-tools.md` — the new verb joins the authoring checklist's
  conformance set.
- `docs/specs/ANTS-3756-roadmap-store-schema.md` — the schema spec gains the
  tenth table.
- `CHANGELOG.md` — one `Added` entry on ship.
- `tool_info {catalog:true}` — the live verb catalogue picks the verb up
  automatically; no hand edit.

## Cold-eyes loop log

| Loop | Date | Lanes | Q-count | Outcome |
|---|---|---|---|---|
