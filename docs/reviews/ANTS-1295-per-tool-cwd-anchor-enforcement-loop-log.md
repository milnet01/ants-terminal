# ANTS-1295 — Per-tool cwd-anchor enforcement on path-accepting MCP tools — review loop log

Record for [`../specs/ANTS-1295-per-tool-cwd-anchor-enforcement.md`](../specs/ANTS-1295-per-tool-cwd-anchor-enforcement.md) § 11. Cold-eyes review log.

## Cold-eyes loop log

**Loop 0 (initial draft, 2026-05-14):** Drafted above.

**Loop 1 — VERIFY against current source (2026-05-14):**
- ✅ `validatePath` defined at `remotecontrol.cpp:1237`; called at
  1424 (`runLogOp`) and 1537 (`runDiffOp`).
- ⚠ Found stale line ranges:
  - `cmdFileOutline` inline block: claimed 1124–1154, actual
    **1123–1156**. ANTS-1253 comment at line **1107**, not 1124.
    → Fixed.
  - `cmdWorkspaceSearch` lane block: claimed 894–921, actual lane
    sub-block **905–920** (the wider root-check span is 894–920).
    `bad_lane` emit sites at **896, 909, 917**. → Fixed.
- ✅ `cmdIndieReviewCorroborate` at 2143; engine call to
  `corroboratedFindingsFromDir` at 2180 anchors (INV-3 at
  `indiereviewengine.cpp:430`) but returns `{}` silently on
  escape — confirmed.
- ✅ `cmdDebtSweepApplyFix` at 2414; engine `applyMechanicalFix`
  at `debtsweepengine.cpp:635` does `projectPath + '/' + finding.file`
  with **no anchor check**. Confirmed unguarded vector.
- ✅ `cmdColdEyesCrossDocDiff` at 2944; delegates to engine which
  re-delegates to `corroboratedFindingsFromDir` (same anchor).
- ✅ `cmdSubsystem` lane is a parsed-set member from
  `SubsystemMap::cachedLanes(CLAUDE.md)`; safe.
- ⚠ INV-2 / WI-4 had a "six vs seven" inconsistency (six tools,
  seven call-sites because git_state has two). → Fixed.

Status: spec consistent with current source after the four edits.

**Loop 2 — Internal consistency (2026-05-14):**
- INV-1..10 reviewed; no contradictions between them.
- Every INV mapped to at least one test case:
  - INV-1 ↔ WI-3 (no inline anchor pattern remains)
  - INV-2 ↔ WI-4 (call-site count)
  - INV-3 ↔ PV-4/5 (rejection envelope shape) + WI-5 (`bad_lane`
    retired)
  - INV-4 ↔ tested via real `QTemporaryDir` (symlink-resolving
    semantics inherent in `QFileInfo::canonicalFilePath`)
  - INV-5 ↔ PV-9 (non-existent escape rejects)
  - INV-6 ↔ PV-6/7 (control char + backslash)
  - INV-7 ↔ PV-3 (dash-prefix → `./` argvForm)
  - INV-8 ↔ PV-1 (empty rawPath neutral)
  - INV-9 ↔ design constraint, caller-enforced; not separately
    testable in the helper unit
  - INV-10 ↔ design constraint; perf measured by absence of new
    static state in the new TU
- Test plan covers every reject reason in the § 6 table.
- Migration § 8 lists the only contract-breaking change
  (`bad_lane` retirement) and gives the rationale.

Status: **CLEAN.** Ready to implement.

---

*End of spec.*
