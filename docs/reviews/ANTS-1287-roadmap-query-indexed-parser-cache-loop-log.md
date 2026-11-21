# ANTS-1287 — Roadmap-query indexed parser cache — review loop log

Record for [`../specs/ANTS-1287-roadmap-query-indexed-parser-cache.md`](../specs/ANTS-1287-roadmap-query-indexed-parser-cache.md) § 11. Cold-eyes review log.

## Cold-eyes loop log

### Loop 1 — 2026-05-14

Pre-loop anchor verification:
- `ROADMAP.md:5228` bullet present (✓ verified via grep).
- `parseBullets` at `roadmapdialog.cpp:528` (✓).
- `cmdRoadmapQuery` at `remotecontrol.cpp:598` (✓).
- Cache members at `remotecontrol.h:217-221` (✓).
- `kRoadmapCacheTtlMs = 100` at `remotecontrol.h:221` (✓).
- ROADMAP.md line count: 10,822 (✓ `wc -l` 2026-05-14).
- `slugifyHeading` at `roadmapdialog.cpp:485` (✓).
- `uniqueSlug` at `roadmapdialog.cpp:511` (✓).
- `headingLevel` at `roadmapdialog.cpp:218` (✓).

Findings fixed inline:
- **HIGH-1.** § 6.1 claimed "~200 section headings" — actual is 149
  ATX headings of any level (17 H2 + 132 H3 + 1 H1 + 18 H4).
  Corrected. Section count for index is now stated as 17 + 132 = 149
  (excluding H1/H4 per INV-3).
- **HIGH-2.** § 6.2 RAM bound revised: 24 KiB → 12 KiB given the
  smaller heading set.
- **MED-1.** § 6.1 had hand-wavey "5-15 ms" numbers for parseBullets
  and "0.5 ms" for slice parsing — unmeasured. Replaced with an
  order-of-magnitude argument that does not depend on a specific
  µs/ms ratio; engine unit test (§ 5.1) will produce informational
  telemetry, not gating thresholds.
- **MED-2.** INV-9 phrasing implied buildIndex skipped non-heading
  lines entirely. Clarified that buildIndex still walks every line
  (line split is unavoidable), but per-line work is reduced to four
  `startsWith` compares — no allocations, no regexes.

### Loop 2 — 2026-05-14

Post-fix cross-doc + theme + dedup + RAM + build-cost sweep.

- Cross-doc: § 2.1 wire shape mirrors ANTS-1247's `{ok, error, code}`
  envelope and INV-11 hygiene rule. CHANGELOG entry to be slotted
  above the ANTS-1284 row on ship.
- Theme: matches the ANTS-1284 / ANTS-1290 spec template (11 sections,
  numbered INV, REG-N tests, cold-eyes log at the bottom).
- Dedup: § 7 explicitly removes the three file-local statics from
  `roadmapdialog.cpp` rather than copy-pasting them into the engine.
  Single source of truth.
- RAM: § 6.2 states a measured (149-section) figure + worst-case bound.
- Build cost: adds two source files to `ants_core_lib`. roadmapdialog
  loses three static defs and gains one `#include`. Net diff is small.

Loop 2 → **CLEAN.** No outstanding findings.
