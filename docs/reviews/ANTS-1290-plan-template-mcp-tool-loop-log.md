# ANTS-1290 — `plan_template` MCP tool — review loop log

Record for [`../specs/ANTS-1290-plan-template-mcp-tool.md`](../specs/ANTS-1290-plan-template-mcp-tool.md) § 11. Cold-eyes review log.

## Cold-eyes loop log

- **Loop 0** (initial draft, 2026-05-14): drafted by author against
  the writing-plans skill markdown + ANTS-1289 spec template +
  project conventions audit (`.roadmap-counter`, `docs/standards/
  commits.md`, `tests/features/README.md`,
  `claudeintegration.cpp:1873–1913`).

- **Loop 1** (2026-05-14, agent-driven verify pass): completed.
  Verified against `.roadmap-counter` (content `1332\n`, 5 bytes),
  `src/roadmapfoldin.cpp:51–92` (existing `allocateIds` reused),
  `src/config.cpp:135–236` (alternative atomic-write idiom),
  `src/indiereviewengine.cpp:208–228` (path-traversal guard
  precedent), `src/claudeintegration.cpp:1873–1915` (registration
  block location), `tests/features/mcp_verify_changes_tool/test_mcp_verify_changes_tool.cpp:78–103`
  (scan-region delimiter), `CMakeLists.txt:217–236, 254` (lib-name
  resolution: engines live in `ants_core_lib` not `ants_chrome_lib`),
  `CMakeLists.txt:888, 894` (bundle assignment for engine + counter
  tests is `test_audit`), `tests/features/README.md:33–41` (bundle
  list), `docs/standards/commits.md:15–18` (commit-format spec),
  the writing-plans skill markdown (152 lines / 6100 bytes; section
  ranges confirmed), and live task-count probes of existing plans
  (4 / 2 / 5; median 4).
  **2 CRITICAL + 4 HIGH + 3 MEDIUM + 6 LOW** findings — all
  addressed in this revision:
  - CRIT-1 / CRIT-1b: `Config::saveAtomic` /
    `IndieReviewEngine::writeManifestAtomic` are fictional symbols.
    The real reuse target is `RoadmapFoldIn::allocateIds`. § 2.4 +
    INV-3 rewritten to delegate to the existing helper (flock +
    QSaveFile + commit). Side benefit: cross-process concurrency
    is now SUPPORTED via the flock lift (previously documented as
    unsupported).
  - HIGH-1: `~1915` → `1915` (drop false uncertainty in line number).
  - HIGH-2: § 2.5 now flags the `YYYY-MM-DD-` → `<ants_id>-` filename
    deviation from the writing-plans skill with INV-2 rationale.
  - HIGH-3: skill markdown is 152 lines / 6100 bytes (not 153 /
    ~6.0 KiB). § 1 numbers updated.
  - HIGH-4 (initially LOW-7): `ants_chrome_lib` → `ants_core_lib` in
    § 2.6. Verified at `CMakeLists.txt:217–236`.
  - MED-1: bundle assignment confirmed correct (test_audit holds
    `roadmap_fold_in` AND `verify_changes_engine` per
    `CMakeLists.txt:888, 894`). § 5 left as-is; original concern
    rested on misreading the README's topical bundle description.
  - MED-2: task counts in § 8 corrected (4 / 2 / 5 not 3 / 4 / 5);
    grep reproducer added inline.
  - MED-3: § 5 scan delimiter is `result["tools"] = tools;` not
    `tools.append(t);`. Spec now mandates tightening the
    verify_changes test's `block_end` in the SAME commit, with
    the exact code pattern given (`find("// ANTS-1290")` first,
    fall back to `result["tools"]`). Pattern mirrors ANTS-1289
    loop-2 CRIT-2 fix.
  - LOW-4: response-shape example uses `ANTS-9999` placeholder
    instead of `ANTS-1290` (avoids the recursive-example
    confusion).
  - LOW-6: § 7(a) caveats the vendor-cache version-scoped
    re-clobber on plugin upgrade.
  - LOW-8: § 8 ANTS-1107 cross-reference left as-is (the deprecation
    notion is documented in ROADMAP.md ANTS-1107 entry; spec
    inherits cleanly).
  - LOW-9: § 5 test 7 names the helper (`RemoteControl::cmdPlanTemplate`)
    and the round-trip path (`QJsonDocument::fromJson`).
  - LOW-10: § 3 INV-6 + § 5 test 6 already covered the clamp
    correctly; no fix needed.
  Sign-off ready after these edits (verified by re-reading the
  changed sections; no new internal contradictions surfaced).

- **Loop 2** (2026-05-14, agent-driven verify pass): **CLEAN.**
  Re-verified all loop-1 fixes against current source:
  `RoadmapFoldIn::allocateIds` signature + 5 s flock budget
  (100 × 50 ms) at `src/roadmapfoldin.cpp:35–92`; `ants_core_lib`
  at `CMakeLists.txt:217–236`; verify_changes test scan delimiter
  + insertion-site fix pattern byte-identical to ANTS-1289 loop-2
  CRIT-2; skill markdown determinism (only `YYYY-MM-DD-` is
  time-of-day-sensitive, explicitly diverged from); ROADMAP-1290
  scope match. **0 CRITICAL + 0 HIGH + 0 MEDIUM + 2 LOW**
  findings — both LOWs observational (loop-1 log lists
  discontiguous finding IDs; `corroboratedFindingsFromDir`'s
  looser path-guard rule is correctly NOT cited). No action.
  Spec is sign-off ready.
