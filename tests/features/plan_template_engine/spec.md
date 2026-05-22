# plan_template_engine — feature contract

Human contract mirror of `docs/specs/ANTS-1290.md` § 3 invariants. The
companion `test_plan_template_engine.cpp` exercises each invariant
against a synthetic project tree under `QTemporaryDir`.

## What this test guards

`PlanTemplateEngine::buildPlan` is the pure-function entry point for
the `mcp__ants__plan_template` MCP tool. It emits a project-
conventional implementation-plan skeleton from caller-supplied
`PlanOptions`, optionally writing it to `docs/plans/<id>-<feature>.md`.

The test ensures the engine:
- rejects malformed feature names (INV-1)
- produces byte-identical output for identical inputs (INV-2)
- mutates `.roadmap-counter` atomically via the shared
  `RoadmapFoldIn::allocateIds` helper (INV-3)
- enforces the strict-below path guard on `save:true` (INV-4)
- refuses to overwrite existing plans (INV-5)
- clamps `task_count_hint` to `[1, 12]` (INV-6)
- performs zero disk writes on `save:false` when `ants_id` is
  provided (INV-9 dry-run)
- rejects a malformed `ants_id` (traversal, embedded slash, wrong
  shape) with `bad_args` before it reaches the filename / skeleton,
  while accepting well-formed project-prefixed ids and an empty id
  (INV-10, ANTS-1838)
- maps `AntsIdSource` to the lowercase JSON key strings the MCP
  response uses

## What this test does NOT cover

- INV-7 (schema round-trip) — covered by the MCP-layer test in
  `tests/features/mcp_plan_template_tool/`.
- INV-8 (RAM bound) — implicit in the deterministic skeleton-size
  bound; no runtime probe.
- The skill-text update (§ 7 (a)) — vendor-cache file, out of tree.

## Bundle

`test_audit` — same bundle as `test_verify_changes_engine.cpp` and
`test_roadmap_fold_in.cpp` (engine-style pure-function tests that
exercise the `ants_core_lib` engines).
