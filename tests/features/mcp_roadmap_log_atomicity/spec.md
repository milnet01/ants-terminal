# roadmap_log two-stage commit — atomic-write rollback seam (ANTS-1433)

`cmdRoadmapLog` (op:"append") commits two files in sequence:

1. `ROADMAP.md` — the spliced bullet, via `QSaveFile`.
2. `.roadmap-counter` — the bumped high-water mark, via `QSaveFile`.

If step 2 fails *after* step 1 succeeds, the on-disk state desyncs:
the appended bullet carries `ANTS-<newId>` but the counter still
reads `newId - 1`. The next `roadmap_log` allocates `newId` again →
**duplicate IDs** in ROADMAP.md (ambiguous flip locators,
`roadmap_query` `duplicate_ids[]` noise).

Before ANTS-1433 there was zero failure-injection coverage for any of
the project's ~10 `QSaveFile` call sites. This is the v1 seam, scoped
to the highest-exposure two-stage write (`cmdRoadmapLog`). v2 extends
the pattern to the other sites (session_memory, plantemplate,
debt-sweep, settings, config, claudeallowlist) — out of scope here.

## Fix

1. **Rollback.** When the counter commit fails, restore `ROADMAP.md`
   to its pre-splice content (the `markdown` string the verb already
   read before splicing) via a fresh `QSaveFile`. The operation is
   now all-or-nothing: either both files advance, or neither does.

2. **Test seam.** A process-global `g_forceCounterCommitFail` flag
   (default false), flipped only by
   `RemoteControl::setForceCounterCommitFailForTest(bool)`. When set,
   the counter `QSaveFile` is `cancelWriting()`-dropped instead of
   committed — exactly the on-disk effect of a real commit failure
   (original `.roadmap-counter` untouched). No IPC/MCP verb reaches
   the flag, so production attack surface is nil; the single
   always-false branch on the rare counter path is free.

   The seam uses the codebase's existing always-compiled `*ForTest`
   convention (`setVerifyInFlightForTest`, `cmdVerifyChangesWithRoot`,
   `putVerifyCacheForTest`) rather than the `#ifdef ANTS_TEST_HOOKS`
   build flag the ANTS-1433 roadmap entry sketched. Reason:
   `remotecontrol.cpp` ships inside the shared `ants_core_lib`, which
   both the production binary and the test bundle link — a build-time
   define would either leak into production or force a separate
   recompile of a heavyweight TU. The runtime flag avoids both.

3. **Test entry point.** `cmdRoadmapLogAppendForTest(req)` drives the
   append path against a synthetic `caller_cwd` (a `QTemporaryDir`)
   without standing up a `MainWindow`, mirroring
   `cmdVerifyChangesWithRoot`. `cmdRoadmapLog` keeps the op-dispatch
   and `m_main` guard and delegates the append body to the new
   `cmdRoadmapLogAppend`.

## Invariants

- **INV-1** (control) The test seam drives the append path end-to-end
  with `RemoteControl rc(nullptr)` and no `MainWindow`. A clean run
  (flag off) against a root pre-seeded with `.roadmap-counter` = 100
  returns `ok:true` with `id == "ANTS-0101"`, advances the counter
  file to `101`, and splices the new bullet into `ROADMAP.md`. Proves
  the seam exercises the real write path, not a stub.

- **INV-2** (counter integrity) With the flag on, the verb returns
  `{ok:false, code:"counter_write_failed"}` and `.roadmap-counter` is
  left byte-unchanged at its pre-call value (100) — no desync, so the
  next allocation cannot reuse the id.

- **INV-3** (ROADMAP rollback) With the flag on, `ROADMAP.md` is
  restored to its exact pre-call bytes: it contains neither the new
  id (`ANTS-0101`) nor the new headline, and is byte-identical to the
  snapshot taken before the call.

## How to verify pre-fix code fails

The rollback (INV-3) and the typed-code refusal under injection
(INV-2) are new behaviour. Against pre-ANTS-1433 source the seam
(`setForceCounterCommitFailForTest`, `cmdRoadmapLogAppendForTest`)
does not exist, so the test fails to compile — the strongest possible
"fails on old code" signal. The INV-1 control passes on both old and
new (it exercises only the pre-existing append path through the new
entry point).

## Out of scope

- Failure-injection for the other 9 `QSaveFile` sites (v2 sweep).
- Forcing the *ROADMAP.md* commit (step 1) to fail — that path
  already returns `roadmap_write_failed` before the counter is
  touched, so there is nothing to roll back.
- Concurrency between two `roadmap_log` callers — the `.roadmap-counter`
  predictable-path hardening is tracked separately under ANTS-1380.
