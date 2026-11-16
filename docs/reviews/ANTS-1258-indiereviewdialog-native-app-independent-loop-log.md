# ANTS-1258 — review loop log

The `review-contract` rows for [`docs/specs/ANTS-1258-indiereviewdialog-native-app-independent.md`](../specs/ANTS-1258-indiereviewdialog-native-app-independent.md).

## Cold-eyes loop log

- **Loop 1 (2026-05-21, independent cold reviewer + author verification).**
  Folded: (a) **§ 2.2 brief-component order corrected** — the engine's
  `assembleBriefForDispatch` emits source bodies → ROADMAP slice → FP
  block → standards-docs-**last** (`indiereviewengine.cpp:262/265/286/300`),
  not the bodies → standards → FP → ROADMAP order the draft listed
  (CRITICAL); (b) Status line moved to the standard's
  `spec draft (DATE)` lifecycle form (`docs/standards/specs.md § 5.6`);
  (c) `§ 2.4` cross-ref fixed to ANTS-1722 § 2.3; (d) added the
  `Layman:` gloss the standard requires for user-facing specs
  (`specs.md § 5.2`). All other concrete citations (engine signatures,
  `corroboratedFindings` minLanes, `dispatchOne` non-re-entry, base
  services, menu wiring `mainwindow.cpp:1525-1533`, `ants::falsepos`
  API) verified accurate against current source. Clean after fold.
- **impl (2026-11-16, amendment; no reviewer dispatched)** — ANTS-6524: INV-9 caps the synthesis prompt at kSynthesisPromptCapBytes; § 4 names the budget. Test: `IndieReviewEngine.Ants6524SynthesisPromptIsCapped`.
