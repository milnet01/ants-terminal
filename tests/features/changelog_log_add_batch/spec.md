# ANTS-2044 — changelog_log op:add_batch

## Background

`changelog_log` (ANTS-1548) writes one Keep-a-Changelog entry per call.
Closing a multi-item release therefore needs one call per entry — the
same RcGate-serialised file means they can't even parallelise. Observed
closing ANTS-2038..2043: five sequential `op:add` calls. `roadmap_log`
already solved the analogous problem with `op:append_batch` (ANTS-1879):
N entries, one read + one atomic commit.

`op:add_batch` takes `entries[]` and writes them all under one read +
one `QSaveFile` commit, reusing `ChangelogLog::insertUnreleasedEntry`
per entry. Each entry is resolved by auto-detected mode: an entry with a
`summary` is an `add` (needs `category` or `kind`); an entry with only an
`id` is an `add_from_roadmap` (pulls the cited bullet's headline +
Layman). Per-entry validation failures land in `skipped[]` while the
rest still apply — parity with `append_batch`.

Entries are inserted in reverse (ANTS-4854), so the on-disk result READS in
input order. Until then they were applied forwards, which made the result
byte-identical to N sequential `op:add`/`op:add_from_roadmap`
calls — and put the LAST input entry on top, so a batch came out backwards
from the array its author wrote. INV-4 owns the reasoning.

## Invariants

### INV-1 — a clean batch applies every entry under one commit

`op:add_batch` with N valid entries returns `ok:true`,
`applied_count == N`, an empty `skipped[]`, and every rendered bullet is
present in CHANGELOG.md after a single write.

### INV-2 — mixed modes in one batch

A batch mixing an `add` entry (`summary` + `kind`) and an
`add_from_roadmap` entry (`id` only, with a ROADMAP.md present) resolves
both: the `add` bullet carries its summary, the `add_from_roadmap` bullet
carries the cited headline and its Layman body.

### INV-3 — a bad entry is skipped, the rest apply

A batch where one entry is invalid (e.g. neither `summary` nor `id`, or a
`kind`/`category` that maps to no canonical category, or an `id` not in
ROADMAP) returns `ok:true` with that entry in `skipped[]`
(`{index, code, error}`) and the valid entries applied.

### INV-4 — the RESULT reads in input order (ANTS-4854)

`add_batch` of `[E0, E1, E2]` into one category leaves the file reading
E0, E1, E2 top-down. Entries are applied in REVERSE, because each insert
prepends to the top of its category.

**This replaces the original sequential-equivalence contract, which said
E1 ends up above E0.** That was true of the code and satisfied
byte-identity with N sequential `op:add` calls — and it meant a batch came
out backwards from the array the caller wrote. Byte-identity with N
separate calls is a property no caller checks; the file's order is one
every caller reads. A batch is one act with an order its author wrote
down, where N calls are N acts. Same entries, same categories, same count
either way.

`applied[]` and `skipped[]` are still emitted in INPUT order, whatever
order the writes ran in: both are keyed by `index` so a caller can line
them up against its own array.

### INV-5 — empty / missing entries refuses bad_args

`op:add_batch` with a missing or empty `entries` array (and a missing
non-array `entries`) refuses with `code:"bad_args"` — no file write.

### INV-6 — top-level refusals match the single op

`add_batch` reuses the single op's caller_cwd / changelog resolution: a
missing `caller_cwd` refuses `missing_field`; a project with only a YAML
changelog refuses `format_mismatch` (ANTS-2040); a project with no
changelog of any kind refuses `no_changelog`. These are batch-level
refusals (no `skipped[]`).

### INV-7 — (ANTS-2136) dry_run previews without writing

`dry_run:true` resolves the would-be insert and returns it WITHOUT
touching CHANGELOG.md, for both the single op and `add_batch`. The
envelope carries `dry_run:true`, the resolved `category`, `line`, a
`bytes` field (would-be file size, replacing `bytes_written`), and — for
the single op — the rendered `bullet`; `add_batch` echoes the same
`applied[]`/`skipped[]`/`applied_count`/`skipped_count` it would commit.
After a dry_run the file is byte-identical to its pre-call content.
Parity with `roadmap_log` dry_run (flip / append / create_section).

## Test plan

Behavioural against `cmdChangelogLog` (op:add_batch; m_main-independent)
over a `QTemporaryDir` project (CHANGELOG.md + optional ROADMAP.md),
mirroring the `changelog_log_writer` harness. No real CHANGELOG.md. INV-3
and INV-5 FAIL against pre-fix code (the op is unknown → `bad_op_combo`).
