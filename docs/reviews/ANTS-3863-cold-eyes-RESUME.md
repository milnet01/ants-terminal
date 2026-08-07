# ANTS-3863 cold-eyes — run state (loop 1 dispatched, NOT yet verified)

**Status:** loop 1 lane A returned; findings below are **raw lane output, not
verified**. Nothing has been fixed from them yet. No loop-log row written.

**Do NOT re-dispatch a review to rediscover these.** A fresh loop costs a full
agent dispatch to regenerate what is already written here. Verify these against
current source, fix the verified ones, then continue the loop from Phase 3.

**Document under review:** `docs/specs/ANTS-3863-pre-read-dispatch.md`
**Genre:** spec. **Loop cap:** 3 (default). **Loops run:** 1 (one lane).

## Rebuilding the context packet

The loop-1 packet lived in this session's scratchpad and does not survive a
session change. To rebuild it, the recipe is: `references/review-brief.md`
verbatim, then bounded windows of — `src/roadmapsource.h` 28-157,
`src/roadmapsource.cpp` 14-45 / 117-200 / 332-372, `src/roadmapparse.cpp`
967-1035 (`detectRoadmapFormat`), `src/remotecontrol_roadmap_query.cpp` 40-105
and 1448-1490, `src/remotecontrol_terminal.cpp` 1372-1417,
`src/remotecontrol_roadmap_log.cpp` 232-250 and 4103-4116,
`src/roadmapdialog.cpp` 550-608, `src/roadmapdialog.h` 479-496 — plus quoted
passages from ANTS-3815 § 2.4 / § 6 / INV-6 and ANTS-3793 § 2.2 / INV-1.

## Deterministic checks — already run, all clean (do not re-run)

`spec_lint` 0 · `doc_integrity` 0 · `doc_citations` 0 stale · `doc_dedup` 0 pairs
involving this doc · `spec_query` parses (7 invariants) · `doc_symbols` 61
unresolved, all triaged as forward references / gtest names / Qt types.

## Lane A findings — RAW, PENDING VERIFICATION

### CRITICAL

1. **[dim 7] § 2.4 vs § 2.5 contradict on the cached-parse path.** § 2.4's table
   row says `remotecontrol_roadmap_query.cpp | 6 | same, except the cached-parse
   path (§ 2.5)`, while § 2.5 says the cache logic "is untouched … makes the
   *miss* cheap". If that site is excepted, the saving never reaches the largest
   consumer; if it is not, the table is wrong. Compounding: the same `markdown`
   feeds the section paths below it, so only the `section.isEmpty()` branch can
   go lazy, which the spec never says.
   *Proposed fix:* state which of the 6 sites change, and that the cached read
   becomes `RoadmapText::fromFile(path)` with `full()` on the section branch.

### HIGH

2. **[dim 2] § 2.3 "exactly one use of the body" is wrong for the dialog.**
   `roadmapdialog.cpp` has **three** `return RoadmapParse::parseBullets(markdown);`
   sites (empty root, `!m_roadmapStore`, final fall-through). An implementer
   converting "the one site" leaves two uncompiled.
3. **[dim 15] INV-7's grep cannot match — it returns zero before any change.**
   `grep` is line-based and `[^)]*` cannot cross newlines; the declarations put
   `const QString &markdown,` on its own line. The invariant is unfalsifiable.
   *Fix:* multiline matcher (`rg -U`), and state the pre-change expected count.
4. **[dim 4] INV-6 and INV-5 require incompatible implementations.** INV-6 wants
   `bytesRead()` == file size exactly once; INV-5's *Breaks when* describes
   `full()` re-reading after the prefix. § 2.1 declares neither a retained handle
   nor an accounting rule.
5. **[dim 5] INV-5's byte-identity is undefined without an open mode.** Every
   existing site opens `ReadOnly | Text`; that flag is exactly what changes CRLF
   handling, so "what the site reads today" ≠ "a direct `readAll()`" on the CRLF
   fixture INV-5 names. *Fix:* pin the open mode in § 2.1.
6. **[dim 5] § 2.2's "three functions that wrap it" omits the fourth caller.**
   `RoadmapDialog::storeLegend()` is dialog code, not seam code, so INV-1's
   *Breaks when* ("any seam function") never forbids it reading the body.

### MEDIUM

7. **[dim 9] § 4's RAM figure ignores UTF-16.** `QString` is UTF-16, so 21,046
   UTF-8 bytes are ~42 KiB in memory, not 21 KiB. Ceiling is ~54 KiB, not "well
   under" 64 KiB — and INV-1's 64 KiB assertion rests on that margin.
8. **[dim 15] INV-1's 64 KiB threshold is asserted, never derived**, and nothing
   bounds a line's length. *Fix:* derive it from the fixture, or assert
   `bytesRead() < fileSize/10`.
9. **[dim 1] § 4 and § 6 restate figures § 4 itself says are stated once in § 1.**
   The doc breaks its own rule three times.
10. **[dim 4] § 2.4 says "five definitions removed by hand" and then names six.**
    The 25 is derived by subtracting them, so the arithmetic is unreproducible.
11. **[dim 10] Deferring the body read opens a TOCTOU window.** The cache stamps
    `m_roadmapCacheMtimeMs` before `full()` runs, so prefix and body can come
    from different file versions. No edge case covers a concurrent writer.

### LOW / INFO

12. **[dim 4] § 2.1 "four dispatch-taking functions"** reads as the whole change
    set, but § 2.3 changes two more wrappers and INV-7 names five symbols.
13. **[dim 5] `openFailed()` is undefined for `fromMemory`**, and covers open
    failure only — a mid-read I/O error has no defined outcome.
14. **[dim 5] § 8 amends ANTS-3815 § 2.4's "live read is not removed" sentence**
    but not that same paragraph's naming of `storeProjectRoot()` as a caller —
    the very error § 1 corrects.
15. **[dim 9] § 5 "latency, downward" carries no numeric target** for a
    performance refactor.
16. **[dim 15] INV-2's test calls `detectionPrefix()` on both sides**, so the
    whole file is never classified — the claim and the test disagree.

## Lane A open questions (orchestrator to resolve with one lookup each)

- Does `RoadmapDialog::loadRoadmapMarkdown(false)` return exactly
  `m_roadmapPath`'s bytes? § 2.4 assumes so when converting `storeLegend()`.
- § 1's `grep -c 'readAll()'` counts (9 / 18 / 1) were not in the packet.
- Do § 2.4's 6 `roadmap_query` sites include the excepted cached-parse site?

## Already fixed pre-dispatch (do not re-report)

- The fourth caller is `RoadmapDialog::storeLegend()`, not `storeProjectRoot()`
  — found during packet construction, corrected in § 1 and § 2.4.
