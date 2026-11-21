# ANTS-1319 — `/cold-eyes` MCP fold-in — review loop log

Record for [`../specs/ANTS-1319-cold-eyes-mcp-fold.md`](../specs/ANTS-1319-cold-eyes-mcp-fold.md) § 11. Cold-eyes review log.

## Cold-eyes loop log

### Loop 1 — 2026-05-14

Findings:

- **HIGH-1 (accuracy / § 2.1.3):** Inline-reports alternative was
  excluded "the whole point is to avoid the parent-context pile-up"
  but the indie-review tool keeps both paths (XOR). Verified against
  `cmdIndieReviewCorroborate` lines 2152-2196 — both paths exist
  there. **Resolution:** keep cold-eyes as disk-only (intentional
  divergence from indie-review's transitional dual-path; cold-eyes
  is greenfield and we don't need to support the v1 inline path).
  Spec wording revised to clarify it's a deliberate-divergence, not
  a copy-paste mistake.
- **HIGH-2 (consistency / § 2.2):** `BriefManifest::citedCodePaths`
  is added but indie-review's manifest doesn't have it.
  **Resolution:** cold-eyes-specific; not a divergence — indie-review
  cites code at the lane level (`Lane::sourcePaths`) because the
  lane IS the code lane; cold-eyes lanes are doc-centric and source
  citations are extracted from doc text via regex. Different
  semantic. Kept; INV-3 wording updated to clarify.
- **MED-1 (clarity / § 5):** "Sort by ID ascending (deterministic)"
  contradicts "12 most-recently-modified by mtime" in INV-2.
  **Resolution:** INV-2 emits the 12 *most-recently-modified*
  (proxy for "most likely to need re-review"); § 5 then sorts those
  12 by ID for stable presentation order. Clarified in INV-2.
- **MED-2 (completeness / § 7.1):** "+250 LoC" claim unverified.
  **Resolution:** rough estimate; replaced with structural argument
  (mirrors indie-review-engine surface) + deferred actual measurement
  to ship-time.
- **LOW-1 (currency / § 8):** Skill markdown line count (142) is
  measured 2026-05-14 head — could shift before fold-in lands.
  **Resolution:** kept; the structural ratio (skill trims to ~30
  lines, 142 baseline) is the point, not the absolute count.

Status: HIGH/MED fixed inline; LOW accepted-as-is.

### Loop 2 — 2026-05-14

Re-read after Loop 1 fixes:

- Cross-doc against ROADMAP entry: theme/Layman/Kind/Source all
  consistent with bullet at ROADMAP.md:5258-5285.
- Cross-doc against ANTS-1281-indie-review-brief-returns-template.md: brief-manifest shape matches
  (§ 2.2 INV-3 ↔ ANTS-1281 INV-5).
- Cross-doc against ANTS-1282-indie-review-corroborate-accepts.md: cross-doc-diff disk-input shape
  matches (§ 2.1.3 INV-5 ↔ ANTS-1282 INV-1).
- Cross-doc against ANTS-1289 / ANTS-1290 displacement criterion:
  every section satisfies "movable iff core loop is run X, parse Y,
  return Z" (§ 1 closing).
- Theme consistency: emoji marker (📝) matches skill SKILL.md § Phase
  6; heading-date format matches ISO; ROADMAP fold-in block shape
  matches `RoadmapFoldIn` API.
- No duplication with existing engines: `IndieReviewEngine`,
  `DebtSweepEngine`, `RoadmapIndex`, `RoadmapFoldIn`, `AuditEngine`,
  `TokenUsageEngine`, `PlanTemplateEngine`, `SubsystemMap` — none
  cover doc partitioning or doc-centric brief assembly.
- RAM budget < 50 KiB per call (§ 7.2). Within project norm.
- Build cost +8–12 s under JOB_POOLS = compile_pool/3 (CMakeLists.txt
  cap). Acceptable.

Status: **CLEAN**. Ready to implement.
