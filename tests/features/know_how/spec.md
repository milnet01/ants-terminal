# ANTS-6310 — the know-how store and its `know_how` verb

The contract is [`docs/specs/ANTS-6310-know-how-store.md`](../../../docs/specs/ANTS-6310-know-how-store.md).
Its § 5 lists the invariants, and its § 7 names the test for each.

`test_know_how.cpp` drives `KnowHowStore` against a file inside a
`QTemporaryDir`, with a clock the test sets. `test_know_how_verb.cpp` drives
`RemoteControl::cmdKnowHow` under an `XdgGuard` sandbox, and holds the verb's
schema to every argument the handler and the store read: no check common to
every verb does that (ANTS-6310 § 7).

INV-12 is in `tests/features/hook_pack` and `tests/features/mcp_orientation_install`.
