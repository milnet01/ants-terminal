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

Entries are inserted in input order, so the on-disk result is
byte-identical to making the same N sequential `op:add`/`op:add_from_roadmap`
calls (each insert goes to the top of its category, so within a category
the last input entry ends up on top — exactly the sequential behaviour).

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

### INV-4 — input order is preserved (sequential-equivalent)

`add_batch` of `[E0, E1]` into the same category leaves the file
byte-identical to `op:add E0` followed by `op:add E1` — E1 ends up above
E0 (most-recent-first per insert).

### INV-5 — empty / missing entries refuses bad_args

`op:add_batch` with a missing or empty `entries` array (and a missing
non-array `entries`) refuses with `code:"bad_args"` — no file write.

### INV-6 — top-level refusals match the single op

`add_batch` reuses the single op's caller_cwd / changelog resolution: a
missing `caller_cwd` refuses `missing_field`; a project with only a YAML
changelog refuses `format_mismatch` (ANTS-2040); a project with no
changelog of any kind refuses `no_changelog`. These are batch-level
refusals (no `skipped[]`).

## Test plan

Behavioural against `cmdChangelogLog` (op:add_batch; m_main-independent)
over a `QTemporaryDir` project (CHANGELOG.md + optional ROADMAP.md),
mirroring the `changelog_log_writer` harness. No real CHANGELOG.md. INV-3
and INV-5 FAIL against pre-fix code (the op is unknown → `bad_op_combo`).
