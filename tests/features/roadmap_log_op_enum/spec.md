# roadmap_log's published op enum matches its dispatch (ANTS-5254)

An op the dispatcher handles but the schema omits cannot be called by a strict
client; an op the schema lists but nothing dispatches refuses every call.
Both have shipped (set_body, convert, retitle_section).

- **INV-1** Every op name `RemoteControl::cmdRoadmapLogDispatch` compares
  `op` against, minus the aliases `add` and `add_batch`, appears in the
  `op` enum of `roadmap_log`'s `inputSchema` as served by `tools/list`.
- **INV-2** Every name in that enum is one the dispatcher compares against.
- **INV-3** `rotate_minor` is in neither: ANTS-4070 § 2.4 keeps it off until
  ANTS-4081.

The dispatcher side is read from source, scoped to the function's body; the
enum side is the live `tools/list` reply.

*Test:* `test_roadmap_log_op_enum.cpp`.
