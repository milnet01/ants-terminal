# roadmap_divergence_guard — feature contract

Test contract for **ANTS-4141 part 1**: the divergence guard in
`RoadmapWrite::commitAndRender()` (`src/roadmapwrite.cpp`), between the dry
render and the store commit.

`roadmap-data-model.md` INV-3 makes the store primary, and ANTS-3809's write
path renders the store's **whole document** on every op. That design assumes the
store is a superset of the file. When the assumption is false the render is not
reformatting the file, it is replacing it: a bullet the store has never imported
is absent from the render's output, and the publish deletes it with no trace.
Measured under ANTS-4065 D3 on this project's own roadmap — a hand-filed
`ANTS-4146` was gone one `roadmap_log op:annotate` later, and the store had
diverged from the file by ~200 ids. Nothing checked it.

The guard compares the dry render's id set against the ids the files it would
rewrite hold today, and refuses `render_would_drop` rather than publishing.

Same harness as `roadmap_write_half`: each case migrates a small markdown
fixture, so the starting state is one the migration can actually produce, then
drives a `*ForTest` verb entry point. `XDG_DATA_HOME` is redirected per case
(`ants_test::XdgGuard`) so `RoadmapStore::defaultPath()` lands in the sandbox;
`RoadmapStore` is never default-constructed.

The divergence is produced the way the real one is produced — by writing a
well-formed bullet into `ROADMAP.md` by hand after the migration, so the store
has never seen it.

## Cases

| Case | Invariant | Asserts |
|---|---|---|
| `RefusesRatherThanDropping` | INV-1 | A `flip` whose render would drop a hand-filed bullet refuses `render_would_drop`, names the dropped id in `error`, leaves `ROADMAP.md` **byte-identical**, and leaves the store unchanged — the flip did not persist behind the refusal. |
| `DryRunRefusesToo` | INV-2 | `dry_run:true` reports the same refusal. A preview that previewed a publish the real call would refuse would be lying about the one thing it is for. |
| `ConvergedStoreStillPublishes` | INV-3 | With no hand edit the same `flip` succeeds and rewrites the file. The guard must not fire on the path every write takes; a guard that refuses everything is indistinguishable from a broken write path. |
| `MentionInBodyIsNotOwnership` | INV-4 | An **id-less** bullet whose prose cross-references an id the render does not emit is not read as owning it. The guard reads each bullet's leading `[<PREFIX>-NNNN]` slot (`BulletRecord::idToken`), not `BulletRecord::id`, which takes the first bracketed id token anywhere in the body. The mentioned id must be one the render does **not** emit or the case cannot discriminate — measured 2026-08-14, a first draft mentioning a live id passed under both readings. |

## Must-fail-first — run, not asserted

The guard is new, so INV-1, INV-2 and INV-4 have no code to fail against before
it. Each mutation below was applied to the shipped guard, built, the case
observed RED, then reverted (2026-08-14):

| Case | Mutation | Observed |
|---|---|---|
| INV-1 | the `WouldDrop` return replaced by falling through to the commit | the flip published; `ROADMAP.md` came back without `DEMO-0099`, exactly the ANTS-4065 D3 shape |
| INV-2 | the guard skipped under `dryRun` — equivalent to placing it after the dry-run early return | the preview returned `ok` on a write the real call refuses |
| INV-4 | `b.idToken` replaced by `b.id` | the id-less bullet claimed the id it merely cross-references, and a converged store was refused |

## Would break this

- Comparing per file instead of across the render's whole output → INV-3: an
  item rotated from `ROADMAP.md` into an archive reads as a drop.
- Reading `BulletRecord::id` instead of `idToken` → INV-4.
- Placing the guard after the store commit → INV-1's rollback window inverts;
  the store would be ahead of a file it refused to write.
- Running the guard only for `op:flip` → the other seven ops keep the hole.
  It sits at `commitAndRender()` precisely because all eight share it.
