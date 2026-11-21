# ANTS-1289 — `verify_changes` MCP tool — review loop log

Record for [`../specs/ANTS-1289-verify-changes-mcp-tool.md`](../specs/ANTS-1289-verify-changes-mcp-tool.md) § 11. Cold-eyes review log.

## Cold-eyes loop log

- **Loop 0** (initial draft, 2026-05-14): drafted by author.

- **Loop 1** (2026-05-14, agent-driven verify pass): completed.
  Verified against `claudeintegration.cpp` (full file 2177 lines;
  indie_review block starts 1574, debt_sweep block ends ~1869),
  `mainwindow.cpp::setupClaudeMcpProviders` (3622–3856 — last tool
  registration at 3853–3856), `indiereviewengine.cpp:208–228` and
  `:419–437` (the two non-identical path-traversal guards),
  `tests/features/mcp_indie_review_tools/test_mcp_indie_review_tools.cpp:94–121`
  + `tests/features/mcp_debt_sweep_tools/test_mcp_debt_sweep_tools.cpp:93–112`
  (both region-scoped, neither generic),
  `CMakeLists.txt:148` (JOB_POOLS) + `:504` (ANTS_TESTS),
  `CMakePresets.json` (3 presets, 80 lines),
  `/home/ants/.claude/plugins/cache/claude-plugins-official/superpowers/5.1.0/skills/verification-before-completion/SKILL.md`
  (140 lines / 4201 bytes), and live `ctest` output for the regex.
  **2 CRITICAL + 6 HIGH + 5 MEDIUM + 7 LOW** findings — all
  addressed in this draft:
  - CRIT-1: schema-test rides a fictional sweep → § 5 now mandates a
    new `mcp_verify_changes_tool/` bundle.
  - CRIT-2: claudeintegration.cpp line range was fuzzy → fixed to
    1574–1869 (indie_review through debt_sweep blocks).
  - HIGH-3: skill markdown was 4.1 KiB not 3 KiB → § 1 + § 6 updated.
  - HIGH-4: ctest regex needed per-line application caveat → INV-5
    rewritten with the `MultilineOption` note.
  - HIGH-5: auto-detection cmake row priority conflicted with
    `CLAUDE.md` → § 2.5 flipped (CMakeLists+build/ wins; preset is
    fallback for fresh checkouts).
  - HIGH-6: `| tail -20` in § 1 table contradicted INV-3 → table
    stripped, footnote added.
  - HIGH-7: INV-4 claimed identical-guard parity that isn't real →
    rewritten to cite `assembleBriefManifest` as the authority
    (strict-below) and note `corroboratedFindingsFromDir`'s differing
    `==` rule.
  - HIGH-8: skill-displacement claim overstated the unconditional
    savings → § 1 + § 6 + § 7 split into "conditional on skill update
    (a)" vs "unconditional Bash-round-trip saving (b)".
  - MED-9: `ants_verify_engine` link target had no precedent →
    § 2.6 rewritten to use the existing main-lib + test-bundle pattern.
  - MED-10: 200 s/gate timeout too tight → INV-2 default bumped to
    1200 s total (400 s/gate) with empirical justification.
  - MED-11/12/14: output magnitudes called out as "indicative" with
    empirical anchors where available.
  - MED-13: `.ants/` vs `~/.cache/` boundary unenforced → flagged for
    ANTS-1283 spec follow-up.
  - LOW-15: example uses non-preset form — kept (matches CLAUDE.md;
    the alternative would conflict with auto-detection priority).
  - LOW-16: byte-cap vs line-cap winner — INV-3 now states byte cap
    is floor; lines trim from head.
  - LOW-17: traversal test target wording — § 5 phrasing tightened.
  - LOW-18/19/20: minor line-number drift + ROADMAP touch-up flagged.

- **Loop 2** (2026-05-14, agent-driven verify pass): completed.
  Verified against all loop-1 anchor points plus live `ctest` output
  (both pass and forced-fail), `QStandardPaths` + `QProcess::kill`
  + `QJsonDocument` Qt6::Core availability, and dedup vs ANTS-1281 /
  1283 / 1284 / 1290 spec scopes.
  **2 CRITICAL + 6 HIGH + 4 MEDIUM + 4 LOW** — all addressed:
  - CRIT-1 (timeout 600 vs 1200 in 3 places): § 2.1 + § 2.6
    updated to 1200 with `INV-2` cross-references.
  - CRIT-2 (debt_sweep test will break on insertion): § 5 now
    mandates tightening `test_mcp_debt_sweep_tools.cpp` scan region
    in the same commit + describes the red→green sanity check.
  - HIGH-3 (KiB vs K-token unit confusion): § 1 rewritten with an
    explicit per-component table in K tokens, all "KiB" references
    converted to K tokens or kept as "~N.N KiB / ~M K tokens" pairs.
  - HIGH-4 (savings arithmetic unreconcilable): § 1 + § 6 numbers
    now factor cleanly. Headline corrected from ~30–60 K to ~6–24 K.
  - HIGH-5 (`skipped_reason` semantics on timeout): § 2.3 table
    rewritten with "Present when" column; field is documented as
    both gate-not-started AND ran-but-killed.
  - HIGH-6 (INV-6 vs § 4 timeout/skip contradiction): INV-6 rewritten
    with explicit `prior_build.ran && !prior_build.passed` predicate;
    § 4 timeout entry rewritten to defer to that predicate.
  - HIGH-7 (§ 2.5 row 2 `cmake --build --preset` requires prior
    configure): row 2 command updated to
    `sh -c 'cmake --preset=default && cmake --build --preset=default'`
    with rationale paragraph.
  - HIGH-8 (gates list vs object serialisation unspecified): § 2.6
    `VerifyOptions` adds the `gateKey(GateName)` helper with explicit
    "build" / "tests" / "lint" mapping.
  - MED-9 (conditional-field schema annotation): § 2.3 "Present when"
    column added per field; spec confirms `additionalProperties:false`
    + non-`required` allows omission.
  - MED-10 (`gates:{}` with "each gate ran:false" contradiction):
    § 4 reworded — gates object always carries all three entries,
    each with `ran:false` when no command resolves.
  - MED-11/12 (line-number drift): § 11 line ranges updated
    (1574–1874 for the full registration block; 3622–3858 for
    setupClaudeMcpProviders).
  - LOW-13/14/15/16: minor wording tightened; § 7(a) explicitly
    flagged as "tracked separately, not gated on this spec landing."
  Sign-off ready after these edits (verified by re-reading the
  changed sections; no new internal contradictions surfaced).

- **Loop 3** (2026-09-28, review-contract, ANTS-5498 amendment: `all_passed`
  false when no gate ran, `ran_count`, INV-14). Two cold `review-lane`
  lanes, each holding all four questions, one shared context file,
  scrubbed copy. **Q1 0 · Q2 3 · Q3 0 · Q4 1** — verified 4, fixed 4,
  dismissed 1. Q4: INV-14's test named the engine bundle, which cannot see
  `ok` or `ran_count`; both lanes found it. Fixed by declaring
  `VerifyReport::ranCount` (§ 2.6) and splitting the test into an engine
  leg and an envelope leg in `verify_changes_build_cache`. Q2: "every
  reply carries `ran_count`" against § 4's `ok:false` shapes (both lanes);
  now "every `ok:true` reply". Q2: § 4 said a command naming a missing
  binary reports `ran:false`, which the amendment had pinned to
  `ran_count:0`; measured, `/bin/sh -c` starts and exits 127, so the gate
  is `ran:true, passed:false`; § 4 narrowed to empty or absent commands.
  Q2, found while building the packet: the new invariant was first
  numbered INV-10, which the engine test's contract already uses; now
  INV-14, with a tombstone line for INV-10 to INV-13. Dismissed (Q3): the
  § 2.2 example lacks `ran_count`; it already omits `cache_hit` and
  `verify_untrusted`, which ship, and no declared-set test exists, so the
  fix changes nothing built. Open questions settled: `runOneGate` sets
  "command not resolvable" only when `/bin/sh` itself fails to start.
  Lane spend 90.4k and 90.7k tokens cumulative, 3 tool calls each.

- **Loop 4** (2026-09-28, review-contract, loop 2 of this run — the spec
  cap). Two cold `review-lane` lanes, each holding all four questions,
  same brief rebuilt from disk, no mention of loop 3's fixes. **Q1 2 ·
  Q2 3 · Q3 0 · Q4 0** — verified 5, fixed 5, dismissed 0. Q2: § 4's
  empty-commands case gave no `config_source`; measured in
  `loadGateConfig`, a parsed file with no usable gate reports
  `".ants/verify.json"` (INV-1), now stated, with § 2.5's `"none"`
  narrowed to no file and no auto-detect hit. Q2: § 5's test plan had no
  INV-14 entry; test 10 added and the mirror line extended. Q2: INV-7
  requires a schema update for a new field and § 2.2's example lacked
  `ran_count`; added. Q1 (outside the change, fixed at the cap since it
  costs no loop): § 2.5 and § 2.6 lacked `"auto (untrusted-bespoke)"` and
  `verifyUntrusted` (ANTS-1337); Q1 (same): INV-2 named `QProcess::kill()`
  where the engine signals the process group, SIGTERM then SIGKILL. Open
  questions settled clean: a cache hit replays an envelope built by the
  same code, so it carries `ran_count`. **Cap reached; calm cap** — 2 of
  this loop's 5 findings landed on text this run wrote. Share on the
  change being gated (ca944437): 6 of the run's 9 verified findings.
  Status set accepted. Lane spend 92.2k and 92.5k tokens cumulative.
