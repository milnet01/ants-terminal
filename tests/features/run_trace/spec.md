# run_trace — feature contract (ANTS-5299, ANTS-5298's record half)

Genre: spec
Status: active
Describes: current

`run_trace` is the MCP verb that records a review run in a project's trace
index, so no skill writes that row by hand. The row format belongs to the v2
workflow's `standards/documents.md` § The header:

```
| Run | Date | Subject | Lanes | Outcome |
```

The design, and the orchestrator's approval of it, are in
`~/.claude/docs/reviews/v2-mcp-verb-proposal-2026-09-24.md` § V3.

The seam is `src/runtraceverb.{h,cpp}`. It is a separate translation unit
from the handler, `RemoteControl::cmdRunTrace` in
`src/remotecontrol_run_trace.cpp`, because `test_core` links `ants_core_lib`
alone. Every case below drives the seam against a `QTemporaryDir`, with the
clock and the random source passed in.

## Invariants

- **INV-1 — `start` mints the id and writes no tracked file.** The id matches
  `^[GCVEFRAL]-\d{8}-[0-9a-f]{4}$`, and its date is the local date of the
  `now` it was given. The only file it writes is the pending record under the
  state directory. An unknown kind, or an empty or multi-line subject, refuses
  `bad_args`.
- **INV-2 — ids do not collide.** When the random source first yields an id
  the index already carries, `start` draws again and returns a different one.
- **INV-3 — `finish` requires `lanes`.** An absent `lanes` refuses
  `missing_field`. A negative or non-integer value refuses `bad_args`. `0` is
  accepted and written as `0`, never as a blank.
- **INV-4 — `finish` creates the index when it is absent.** The new file
  carries a `Genre:` line and the header row. The row's first field is the
  id: `| <id> | <date> | <subject> | <lanes> | <outcome> |`.
- **INV-5 — a foreign index is refused, never rewritten.** An existing index
  whose table header is not the fixed row refuses `format_mismatch` and leaves
  every file byte-identical.
- **INV-6 — cells cannot break the row.** A `|` in a subject or outcome is
  escaped. A newline in either refuses `bad_args`.
- **INV-7 — every row has its detail file beside it.** A `detail` argument is
  written to `<index dir>/<id>.md`, with a `Genre: record` header added when
  the text declares none. With no `detail`, an existing file there is
  accepted untouched. When the file is also missing, `finish` refuses
  `missing_field`.
- **INV-8 — cost needs coverage beside it.** A `cost` object with no
  `verified` refuses `coverage_required`. A cost with `lanes: 0` refuses
  `bad_args`. Usage is summed only over assistant lines stamped at or after
  `started_at`, and each `message.id` is counted once. A `run-costs.tsv` whose
  header differs from the fixed columns refuses `format_mismatch` and is left
  untouched.
- **INV-9 — one row per id.** A second `finish` on a recorded id refuses
  `already_recorded`. A `finish` on an id with no pending record refuses
  `not_found`.
- **INV-10 — `dry_run` writes nothing.** On `start` and on `finish` alike,
  every file under the root and under the state directory is unchanged, and
  the reply carries `dry_run: true`.
- **INV-11 — `get` reports all three states.** `recorded` returns the row's
  five fields, `pending` returns the started record, and an unknown id
  returns `found: false`.
- **INV-12 — `gate_log` is honoured.** A `.claude/workflow.json` `gate_log`
  key moves the index. A malformed file falls back to the default path and
  says so in `config_warning`. A path escaping the root refuses `bad_path`. The
  root is the nearest ancestor holding `.git`, which is where `gate-record`
  looks.
- **INV-13 — the transcript is named, never guessed.** A `session_id` resolves
  to `<projects dir>/<slug of cwd>/<session_id>.jsonl`. A transcript path
  outside the projects directory refuses. No code path picks "the newest"
  transcript.
- **INV-14 — the verb is registered with the Required contract.** Its schema
  sets `additionalProperties: false`. This case is a source scrape of
  `mcptoolregistry.cpp` and `claudeintegration.cpp`.
