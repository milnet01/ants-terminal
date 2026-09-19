# Feature: append_batch bullets cite their own siblings

## Invariants

**INV-1 — a token becomes the sibling's id.** `{{id:N}}` in a bullet's
`headline`, `body` or `layman` is replaced by the id that bullet N of the
same call receives. N counts the `bullets` array from 0. No token reaches
the published file.

**INV-2 — an unresolvable token refuses the whole call.** A token naming a
bullet that was skipped, or an index past the array, refuses `bad_args`.
The message names the citing bullet, and nothing is written.

## Rationale

ANTS-4580. Bullets filed together often cite each other, and their ids do
not exist until the write returns. Callers predicted them and got them
wrong, or paid one `amend_body` per cross-reference afterwards.

## Test surface

`test_roadmap_append_batch_id_tokens.cpp` migrates a fixture into a
sandboxed store and drives `RemoteControl::cmdRoadmapLogAppendBatchForTest`.
The substitution runs before the store and markdown paths split, so both
see the resolved text.

## Regression history

- **ANTS-4580:** no way for a bullet to cite a batch sibling.
