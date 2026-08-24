# ANTS-4470 / ANTS-4466 / ANTS-4464 — `annotate_batch`, and two envelope truths

## Background

Three defects reported by CC sessions against `roadmap_log`, all in the
flip/annotate verb family and all about what the envelope *says*.

**ANTS-4470** — `op:"annotate"` was the only bullet-writing op with no
batch form. `append_batch` (ANTS-1879) and `flip_batch` (ANTS-1690) were
each filed to remove exactly this cost, so this was an asymmetry rather
than a feature request in the abstract. Closing one roadmap item
routinely means annotating two or three neighbours; measured in the
reporting session, that was three calls, three reads of a 289 KB
ROADMAP.md and three commits where one batch would have done. It also
raced the file watcher the same way N separate flips did before
`flip_batch` existed.

**ANTS-4466** — after a `git checkout --` reverted ROADMAP.md while the
store kept a flip, `op:"annotate"` returned `from_status:"📋",
to_status:"📋"` while the re-render it performed correctly restored ✅.
The envelope described the op's stale **input** rather than its
committed **result**. Nothing was lost; the report was wrong — and it
was wrong in exactly the divergence case where a caller confirming a
write from the envelope most needs it right.

**ANTS-4464** — identical `op:"flip"` calls returned two different field
sets: one carrying `line` / `note_line` / `bytes_written`, the next
carrying `files_written[]` / `items_rendered`. The reported diagnosis
was "one path that dropped four fields". The measured cause is different
and is recorded here because the fix follows from it: they are **two
paths**, markdown-patch and store-render, and ANTS-3793 INV-2 declares
the field difference deliberately — a store has no lines, and resolving
one would mean re-reading the file the render just wrote, which
ANTS-3863 exists to avoid. What was missing was not the fields but the
**statement**: absence was a silence.

## Invariants

### INV-1 — `annotate_batch` appends N notes and flips nothing

`op:"annotate_batch"` with `locators[]` each carrying a `note` appends
every note to its own bullet and leaves every status emoji unchanged.
One read, one atomic write. The envelope carries `op:"annotate_batch"`.

### INV-2 — it is `flip_batch`'s locator shape, unchanged

`annotate_batch` accepts `locators[]` in exactly the form `flip_batch`
takes — `{id|anchor|headline|line_range}` plus the per-locator `note`
that array has carried since ANTS-1690. No second shape to learn.

### INV-3 — `to_status` is refused, not ignored

`op:"annotate_batch"` with a `to_status` refuses `bad_op_combo` and
writes nothing. A caller who passes one means to change status, and
silently dropping it would leave them believing a flip had happened.
Mirrors `op:"annotate"`'s existing refusal.

### INV-4 — a noteless locator is skipped, per locator

A locator with no `note` writes nothing, so it is refused into
`skipped[]` with `missing_field` while every other locator still
applies — this op's failure model. When *every* locator is noteless the
all-failed refusal still fires, preserving the whole-call refusal for
the whole-call mistake.

### INV-5 — no batch-wide `to_status` under annotate

The top-level `to_status` is ABSENT on an `annotate_batch` envelope,
because each item keeps its own status and a single value there could
only be wrong. Each row of `flipped[]` carries its own `to_status`,
equal to that item's unchanged status.

### INV-6 — the store path reports the STORE's status

On a store-backed project, `op:"annotate"` and `op:"annotate_batch"`
report `from_status` / `to_status` from the store — the authority, and
the thing the render just committed — never from the parsed file. Where
the file disagrees, `file_status` carries the file's value so the
divergence is visible instead of being resolved silently in one
direction.

`file_status` rides on the true arm only: it is absent when the two
agree, which is every healthy write. A key present on every write
restating `from_status` is a key nobody reads.

### INV-7 — every flip/annotate envelope names its write path

`write_path` is `"render"` on the store-backed path and `"patch"` on the
markdown path, on both the single-item and batch ops, and on dry-run and
success envelopes alike. It is what turns the declared field difference
of INV-2 (ANTS-3793) from a silence into a statement: a caller reading
`note_line` and getting nothing can now tell *why*.

Named `write_path` rather than the reported `path`: these envelopes
already carry `file`, and `path` is a filesystem word everywhere else in
the verb layer.

### INV-8 — the store path honours `return:"headline_only"`

`post_bullets` is emitted on the store path as it always was on the
markdown path. It was silently missing on migrated projects — the same
divergence class as INV-7, found while fixing it.
