# ANTS-4622 — Cross-session mailbox: a `message` table and its verb

**Status:** accepted, review-contract loops 1 + 2 folded, cap reached (2026-08-22).
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
  from_session    TEXT NOT NULL DEFAULT ''
                    CHECK (length(CAST(from_session AS BLOB)) <= 128),
  created_at      TEXT NOT NULL
                    CHECK (created_at GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z'),
  body            TEXT NOT NULL
                    CHECK (length(body) > 0
                       AND length(CAST(body AS BLOB)) <= 4096),
  acked_at        TEXT
                    CHECK (acked_at IS NULL OR acked_at GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z')
)
```

```sql
CREATE INDEX idx_message_inbox ON message(to_project_id, acked_at)
CREATE INDEX idx_message_from  ON message(from_project_id)
```

The `created_at` GLOB mirrors `history.changed_at` verbatim rather than
inventing a second timestamp convention.

**Both caps are byte-valued, and that is not a detail.** SQLite's `length()`
counts **characters** on a TEXT value, so `length(body) <= 4096` would admit
4096 characters — up to 16 KiB of UTF-8, four times what § 4 budgets — and a
handler validating bytes would accept bodies the constraint then rejects.
`CAST(… AS BLOB)` is what makes the cap mean bytes (verified: 2000 three-byte
characters are refused, 1000 are accepted at 3000 bytes).

**Both indexes ship in the same array as the tables**, and there are two of
them for the reason `ddl[]` states over its own FK block: *"SQLite does not
auto-index foreign keys. Only those whose column does NOT already lead a unique
index or PK."* `to_project_id` leads `idx_message_inbox`; `from_project_id`
leads nothing, and § 2.5 deletes on it. `relationship` is the precedent again —
it carries `idx_rel_src` **and** `idx_rel_dst`, both ends, exactly as its delete
is both ends. The `idx_<table>_<what>` naming is that block's too.

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

**All three ops require the CALLING project to be registered**, because
`from_project_id` is `NOT NULL` and `inbox`/`ack` resolve the caller the same
way. That refusal is `no_project`, which the taxonomy already carries — a
caller in an unregistered checkout must be able to tell *you are not
registered* from *your recipient is unknown*, and one code cannot say both.

Refusal codes: `no_project` (the caller's own root is not registered),
`unknown_project` (the `to` slug resolves to no row), `not_found` (`ack` on an
id that does not exist **or on a message addressed to another project** —
deliberately the same code, so a probe cannot use the refusal to learn that a
message id exists), `inbox_full` (§ 2.6), `bad_args` (empty or oversized body).

Of these, `no_project`, `not_found` and `bad_args` are already in
`docs/standards/mcp-error-codes.md`; **`unknown_project` and `inbox_full` are
new and the standard gains them in the same change** (§ 7). No third new code
is introduced — an earlier draft added `forbidden` for the wrong-recipient
`ack`, which both grew the taxonomy and leaked existence.

**`project_not_registered` is NOT reused for the recipient case, and the
taxonomy is why.** Its entry scopes it to `roadmap_log op:"backfill_dates"`
against a project the store holds no row for, describing itself as that op's
remap of the shared store-op prologue's `op_unsupported`. It is a statement
about the **caller's** project, which here is `no_project`. `unknown_project`
says something the taxonomy has no code for: the caller is fine and the
*addressee* does not exist.

Each `messages[]` entry carries `message_id`, `from` (the sender's
`export_slug`), `from_session`, `created_at`, `body` and `acked_at`. The first
two are load-bearing rather than informational: `ack` takes the `message_id`,
and a reply — which § 5 makes the substitute for threading — needs the sender's
slug to address `to`. An entry without them leaves both paths unusable from an
inbox read.

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

The block is `mail_pending: {unacked_count: <int>, from: [<export_slug>, …]}`
and carries no other key. `unacked_count` is the same name `inbox` returns in
§ 2.3, deliberately — an orient block and a verb reply describing one number
should not spell it two ways — and `from` is the distinct sender list, which is
what makes the block actionable rather than merely alarming.

**Unlike `feedback_pending`, it is not gated on shipping a document.** That
gate exists because feedback triage is the Ants repo's job alone; an inbox
belongs to every registered project.

**The block is emitted only when `unacked_count` is non-zero** — never as a
present-and-zero block. "No mail" here means *no unacked mail*, not *no rows*:
a project that has acked everything still holds rows until § 2.6's prune
reaches them, and emitting a zero block for those 30 days would churn the orient
ETag every session and put a permanent nothing-to-do in front of the reader.
That is ANTS-3631's failure exactly, which is the mistake this section cites two
paragraphs above.

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

- **Acked mail is pruned 30 days after its `acked_at`**, opportunistically on
  any op **that already writes** — `ack`, and `send` without `dry_run` — over
  rows whose `to_project_id` is the calling project. A project prunes its own
  inbox, and an acked message has done its job.

  **Three things are pinned there, and each was ambiguous without it.** The
  **column** is `acked_at`, not `created_at`: a message created 60 days ago and
  acked today has just been read, and deleting it on the next op would destroy
  mail the moment it was received. The **side** is `to_project_id`; scoped to
  `from_project_id` a project would prune rows in *other* projects' inboxes and
  never its own. And the trigger is a **writing** op, because a `dry_run` send
  is an op too and INV-10 forbids a preview from writing anything.

  **What this deliberately does not promise:** a dormant project's acked mail
  survives past 30 days, because nothing is running to prune it. The bound is
  *opportunistic*, not a clock — see § 4.
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
  B's `mail_pending` counts one and A's is **absent**, not present-and-zero,
  which § 2.4 requires for ETag stability. B's leg is what distinguishes a
  working direction predicate from a block that failed to render at all, which
  would also leave A empty. The message is unacked, so an ack filter cannot
  account for A either.
- **INV-5** — Deregistering a project removes its mail from both ends and no
  further. *Test:* A and B exchange messages and B also holds an unrelated
  message from C; deregister A; no `message` row referencing A survives, and
  B's message from C is untouched. The surviving row is what distinguishes a
  scoped delete from a table truncate.
- **INV-6** — The bump is expressed twice and the two agree: a store created at
  version 3 and one climbed from version 2 hold the same `sqlite_master`
  statements — the table **and** both indexes, since all three ship in `ddl[]`.
  *Test:* `tests/features/roadmap_store_upgrade/test_roadmap_store_upgrade.cpp`.
  Its ANTS-3815 INV-3 comparison reads `sqlite_master` **derived rather than
  from a hardcoded table list**, so it covers the new rows without being
  extended — and it compares the **sorted, whitespace-normalised** statement
  set, via `schemaStatements()`'s `out.sort()` and its `norm` callback. So the
  invariant does NOT constrain where in `ddl[]` the new statements sit; what
  breaks it is the rung and the array disagreeing on the text of any one of
  them.
- **INV-7** — Every argument `session_message`'s handler reads via
  `req.value()` is declared in the verb's `inputSchema.properties`, and `op`'s
  enum carries `send`, `inbox` and `ack`. *Test:* the ANTS-4621 guard's shape —
  scrape the handler for its `req.value()` keys and assert each is declared,
  `caller_cwd` excepted as a universal dispatch arg (ANTS-2175 INV-2). Written
  against the source rather than the engine, because no test that drives the
  engine seam crosses the dispatcher.
- **INV-8** — The prune removes acked mail past the TTL and unacked mail at no
  age. *Test:* two rows in the calling project's own inbox, both with a
  `created_at` well past § 2.6's TTL — one unacked, one with an `acked_at` also
  past it — then a writing op by that project. The acked row goes, the unacked
  row stays. **Both legs are required and neither alone is sufficient:** the
  surviving row alone passes against a prune that never runs, and the deleted
  row alone passes against one that ignores `acked_at` and filters on age. The
  fixture must sit inside the scope § 2.6 defines, or the prune never reaches
  either row.
- **INV-9** — A full inbox refuses rather than drops. *Test:* fill a
  recipient to § 2.6's cap, send once more, assert `code:"inbox_full"` and that
  the recipient's row count is unchanged.
- **INV-10** — `dry_run` writes nothing and claims nothing in the past tense.
  *Test:* `send` with `dry_run:true` against an inbox holding **one acked row
  already past § 2.6's TTL** adds no row, deletes none, and returns an envelope
  carrying `would_send` and no `message_id` (ANTS-4463). The aged row is what
  makes the test discriminating: against an empty table a preview that fires the
  prune is indistinguishable from one that does not.

## 4. RAM / build cost

**What is hard-capped: unacked mail.** 500 unacked × 4 KiB body = 1.95 MiB per
recipient; across the 16 registered projects, 31.2 MiB before any `send` is
refused (`python3 -c "print(500*4096*16/1048576)"`). **Those figures depend on
§ 2.2's caps being byte-valued** — under a character cap the same DDL admits
four times as much — and on `from_session` being bounded, which is why it
carries its own CHECK rather than only a default.

**What is NOT hard-capped: acked mail, and the spec says so rather than
implying otherwise.** § 2.6's cap counts unacked rows, so acking frees a slot;
and its prune is opportunistic, so a project that stops calling the verb stops
pruning. A busy project that acks promptly therefore holds up to 30 days of
traffic above the 1.95 MiB, and a dormant one holds whatever it had when it went
quiet. **This is a deliberate trade, not an oversight.** The alternatives are a
background timer (a thread this project does not have, for mail nobody is
waiting on) or a total-row cap that would refuse *delivery* because the
recipient had been slow to tidy up — and refusing to deliver mail is a worse
failure than holding some acked rows longer than advertised. The figure a
capacity decision should rest on is the unacked one; treat the acked tail as
unbounded-but-small and revisit it if a real corpus says otherwise.

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
- **Export and import.** `message` is deliberately NOT serialised by
  `src/roadmapexport.cpp` and gains no `"t"`-tagged record type of its own.
  The reason is structural rather than a judgement about worth:
  **a message references two projects and an export carries one**, so exporting
  a project's mail would import rows whose other end does not exist — the
  dangling reference § 2.5 exists to prevent, arriving through the import path
  instead. `kExportSchemaVersion` therefore does not move. The cost is real and
  accepted: an export/import round-trip loses mail. No id — this is a decision
  not to build it.

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
- `docs/standards/mcp-error-codes.md` — gains `unknown_project` and
  `inbox_full`. Both are new; `no_project`, `not_found` and `bad_args` are
  already catalogued and this spec reuses them unchanged (checked 2026-08-22).
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
| 1 | 2026-08-22 | 3, cold — genre pinned `spec`; one byte-stable shared packet carrying the `roadmapstore` DDL rules, `deregisterProject()`, the upgrade ladder, the PID-identity branch and the `feedback_pending` window | **Q1 3 · Q2 3 · Q3 4** (10 verified / 0 dismissed) | **Ten verified, ten fixed; nothing deferred.** **All three lanes independently found the same Q1**, and it is the run's most consequential: SQLite's `length()` counts **characters** on a TEXT value, so the draft's `length(body) <= 4096` capped 4096 *characters* — up to 16 KiB of UTF-8 — making every figure in § 4 understated by up to 4×, and leaving a handler that validates bytes accepting bodies the constraint then rejects. Now `length(CAST(body AS BLOB))`, verified by the refuting case (2000 three-byte characters refused, 1000 accepted at 3000 bytes). One lane added the other half: `from_session` carried no CHECK at all, so a row was unbounded whatever the body cap said. **The best Q3 was a coupling the roadmap item never named** — `src/roadmapexport.cpp` serialises nine `"t"`-tagged per-project record types, and the draft said nothing about whether mail joins them. It does not, and the reason is structural: a message references TWO projects and an export carries one, so exporting mail would import rows whose other end does not exist — the dangling reference § 2.5 exists to prevent, arriving through the import path. **Two Q3s were one ambiguity with two victims:** *"the next `send` by the same project"* never said which side it scoped, and under the `from_project_id` reading INV-8 passed without exercising anything — the vacuous-fixture trap, in a clause written the same day the drafting rule warning about it was read. The prune now fires on any op by the calling project over its own inbox, which also closes the Q2 where a project that acks and never sends again would never prune while § 4 promised a 30-day bound. **Three of the ten were the spec claiming things about documents it had not re-read:** `forbidden`, `unknown_project` and `inbox_full` were cited *per* `mcp-error-codes.md` and are absent from it, while `no_project` — the exact code for the unregistered-caller case the draft left unspecified — was already there. `forbidden` is dropped outright: reusing `not_found` for a message addressed elsewhere removes a new code and stops a probe learning that an id exists. **Open questions, none of which became findings:** the upgrade harness's ANTS-3815 INV-3 comparison is `SELECT sql FROM sqlite_master … ORDER BY rowid`, derived rather than a hardcoded list, so INV-6 needed its claim widened rather than the harness extended; `forbidden` appears 0 times in `src/`; and no other length CHECK exists in `roadmapstore.cpp` to set a character-vs-byte precedent. |
| 2 | 2026-08-22 | 3, cold — identical brief, packet rebuilt whole from disk; two packet gaps loop 1's lanes had flagged were closed first (the error-code list was headings-only, and § 5 now cites `roadmapexport.cpp`) | **Q1 2 · Q2 3 · Q3 4** (9 verified / 0 dismissed) | **Nine verified, nine fixed. Cap reached (2 for a spec); nothing deferred.** **Five of the nine landed on text loop 1 wrote** — 4a-min's measured pattern, and worth stating plainly rather than in prose: this is a moderately oscillating cap, not a calm one. **The worst was loop 1's own repair being false.** Loop 1 widened INV-6 by asserting the harness compares `sqlite_master` byte-identically `ORDER BY rowid`; that query is the FIXTURE-GENERATION dump, and the live comparison is `schemaStatements()`, which ends `out.sort()` and passes every statement through a `norm` callback. So the comparison is SORTED and NORMALISED, and an implementer taking INV-6 literally would have concluded rowid order can never match between a created and a climbed store and declared the invariant unachievable. One lane found it, a second raised it as an open question, and the source settled it. **Loop 1 also introduced two of its own:** the index it added covered one FK end while `ddl[]`'s own block says SQLite does not auto-index foreign keys and `relationship` carries `idx_rel_src` AND `idx_rel_dst` — both ends, exactly as its delete is both ends; and the export bullet it added counted nine `"t"`-tagged record types where there are ten. All three lanes raised that count as an open question and none filed it, correctly — nothing is built differently either way — so the number was DELETED rather than corrected, since it was carrying nothing. **And loop 1's fix for the vacuous-fixture trap contained the trap again:** pinning the prune's side left its COLUMN unpinned, and under the `acked_at` reading an unacked row has `acked_at IS NULL`, so a pure age filter deletes nothing and INV-8 passed against a broken prune. Three lanes reached that column from three directions. INV-8 now carries two rows and says why neither leg alone is sufficient. **Four were pre-existing and loop 1 never reached them:** the 500 cap counts UNACKED rows, so § 4's 31.2 MiB was never a bound on the table — the spec now states outright which half is hard-capped and which is a deliberate trade; `mail_pending`'s keys were never named though INV-4 asserts against them; "a project with no mail" admitted *no rows* and *no unacked rows*, the second being ANTS-3631's failure the same section cites; and a `dry_run` send is an op, so loop 1's "any op" trigger had previews pruning. **Open questions, none of which became findings:** `project_not_registered` is scoped by the taxonomy to one op's remap of `op_unsupported` and is a statement about the CALLER, so it does not cover an unknown addressee — recorded in § 2.3 because three lanes asked. |
