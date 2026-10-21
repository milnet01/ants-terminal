# Feature: content verb argument bounds

## Invariants

**INV-1 — session_message bounds its inbox page.** `op:"inbox"` caps
`limit` at 200 through `RoadmapStore::inboxPageLimit`, and clamps `offset` to
zero or more, so 0 or a negative limit never returns the whole mailbox. Since
ANTS-5591 it asks for 200; `SessionMessage.Ants5591…` tests the values.

**INV-2 — session_message range-checks `message_id`.** `op:"ack"` refuses a
`message_id` that is not a positive integer within the exact range of a
double before casting it.

**INV-3 — changelog_log caps `add_batch`.** `op:"add_batch"` refuses more
than `kMaxBatchEntries` entries with `bad_args`.

## Rationale

The ANTS-5098 performance pass found all three: an unbounded inbox page, an
unchecked double-to-integer cast, and an uncapped batch array.

## Test surface

`test_content_verb_bounds.cpp` reads `src/remotecontrol_session_message.cpp`
and `src/remotecontrol_changelog.cpp` (located from the test's own path).

## Regression history

- **ANTS-5098:** the three defects above. Locked by this spec.
