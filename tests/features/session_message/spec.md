# session_message — feature contract

Design contract:
[`docs/specs/ANTS-4622-cross-session-mailbox.md`](../../../docs/specs/ANTS-4622-cross-session-mailbox.md).
That document owns the surface, the refusal table and the reasoning; this file
records what the test binary asserts and how.

## What the feature is

The cross-session mailbox: a `message` table in the machine-global roadmap
store, addressed **project to project**, with a two-state acknowledgement.
Before it, the only channel between Claude Code sessions was the
`*_Ants_MCP_Feedback.md` corpus — broadcast, addressed to the Ants maintainer,
and terminating in a roadmap id rather than in a receipt.

## Routing

Every invariant here drives `RoadmapStore` directly, against a store inside a
`QTemporaryDir`.

**Never default-construct `RoadmapStore` in a test.** Its default path resolves
through `RoadmapStore::defaultPath()` to `XDG_DATA_HOME`, which is the
developer's real machine-global store — the mechanism by which ANTS-3856's
fixture row leaked into live data.

`registerProject()` canonicalises the root it is given and refuses a path that
does not resolve, so each fixture project gets a real directory created inside
the temporary dir. A slug-only fixture would be refused before the mailbox was
reached, and the test would pass for the wrong reason.

| Invariant | Route |
|---|---|
| INV-1, INV-2, INV-3, INV-5, INV-8, INV-9 | `RoadmapStore` against a temp store |
| INV-6 | `tests/features/roadmap_store_upgrade/` — created vs climbed |
| INV-4, INV-7, INV-10 | the verb and `session_orient` layer, not this file |

## Why timestamps are parameters

`sendMessage()`, `ackMessage()` and `pruneAckedMail()` take their timestamps
from the caller rather than reading the clock. That matches the store's
existing convention — `appendHistory()` takes `changedAt` — and it is what
makes INV-8 testable at all: the invariant needs rows on each side of a 30-day
TTL, and a store that stamped internally could not be handed one without
sleeping or faking a clock.

## Invariants

- **INV-1** — A well-formed but **unregistered** recipient slug refuses
  `unknown_project` and writes no row. The slug is syntactically valid, so the
  `export_slug` CHECK cannot be what rejects it; only the resolution step can.
- **INV-2** — `inbox` returns only the calling project's mail. Both fixture
  messages are unacked and recent, so neither the ack filter nor retention can
  account for the exclusion — only the recipient predicate can.
- **INV-3** — Confirmation is two states in one nullable column, and `ack` is
  idempotent. The second ack passes a **different** timestamp and the stored one
  must not move: the first ack is the fact. A test that re-acked with the same
  stamp would pass against an implementation that overwrites.
- **INV-5** — Deregistering clears a project's mail from **both ends** and no
  further. Three projects, so a row belonging to neither party survives — that
  survivor is what distinguishes a scoped delete from a table truncate.
- **INV-8** — The prune removes acked mail past the TTL and unacked mail at no
  age. **Two rows, and neither leg alone is sufficient:** the surviving row
  alone passes against a prune that never runs, and the deleted row alone
  passes against one that ignores `acked_at` and filters on age. A companion
  test pins the scope — pruning as the *sender* must not reach the recipient's
  inbox, which is the reading under which INV-8 would pass without exercising
  anything.
- **INV-9** — A full inbox refuses `inbox_full` rather than dropping the
  oldest, and the row count is asserted unchanged across the refusal. The test
  then acks one message and sends again, because the cap counts **unacked**
  rows and a cap that counted every row would fail that leg.

## Also asserted, and not from the design contract's invariant list

- **The body cap is byte-valued.** 2000 three-byte characters (6000 bytes) are
  refused while 1000 (3000 bytes) are accepted. SQLite's `length()` counts
  **characters** on a TEXT value, so a character-valued cap would admit up to
  four times the budget § 4 states — and a handler validating bytes would accept
  bodies the constraint then rejected. Found by all three lanes of the design
  document's first review loop.
- **`ack` on another project's message is `not_found`**, the same code an
  absent id gets. Deliberately indistinguishable, so a probe cannot use the
  refusal to learn that a message id exists.

## Out of scope

INV-4 (`session_orient`'s `mail_pending` block), INV-7 (the schema declares
what the handler reads) and INV-10 (`dry_run` writes nothing) belong to the
verb and orientation layer and are not driven from here.
