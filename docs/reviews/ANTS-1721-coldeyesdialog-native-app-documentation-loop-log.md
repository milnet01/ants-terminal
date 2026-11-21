# ANTS-1721 — `ColdEyesDialog`: native in-app documentation review — review loop log

Record for [`../specs/ANTS-1721-coldeyesdialog-native-app-documentation.md`](../specs/ANTS-1721-coldeyesdialog-native-app-documentation.md) § Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-05-21, independent cold reviewer + author self-review).**
  No blockers. Folded: (a) `ants::falsepos` prior-FP reference made
  precise (`loadEntries`/`filter`/`formatForBrief`, `src/falseposledger.h`);
  (b) per-file vs 200 KiB sum-gate interaction made explicit in § 2.2
  (the sum gate, not the 64 KiB per-file cap, guarantees the bound);
  (c) `performFoldIn` override built from the base helpers
  (`allocateFoldInIds`/`insertFoldInBlock`), aligning with ANTS-1727's
  subclass-owns-fold-in model. Clean pass.
- **Shipped note (2026-05-21).** § 2.2.3 lists `m.brief` as a user-prompt
  component, but `ColdEyesEngine::assembleBriefManifest` already embeds
  the FP block + prior-fix block AND carries "read each doc via your Read
  tool" instructions — both wrong for a raw inlined-bodies endpoint (no
  Read tool; duplicate FP block). The implementation therefore composes
  its own prompt from the manifest's *structured* fields (`summary`,
  `crossReferenceDocs`, `staleCitations`) rather than reusing `m.brief`
  verbatim, calling `formatForBrief` once and rendering prior fixes
  itself. Net effect matches § 2.2's component list with no duplication.
- **Sign-off (2026-05-21, user).** Approved. Folded the "smarter slice"
  direction: cross-reference contracts now use
  `BriefDispatch::inlineRelevantSections` (ANTS-1727 INV-17) with
  lane-derived keywords instead of a blind 32 KiB head-truncation
  (§ 2.2, INV-2, INV-4 updated). Status → accepted.
- **Amendment (2026-09-10, ANTS-2011, user decision).** § 2.4's
  re-review now runs cold and gains the loop log; § 2.2 passes no prior
  fixes; INV-6 inverted; INV-9 added. Records what ANTS-2011 built.
