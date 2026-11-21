# ANTS-2126 — `roadmap_log` heading-format writer (`#### Pass N.M`) — review loop log

Record for [`../specs/ANTS-2126-roadmap-log-heading-format-writer.md`](../specs/ANTS-2126-roadmap-log-heading-format-writer.md) § Cold-eyes loop log.

## Cold-eyes loop log

Two cold lanes per loop (A: spec-vs-code accuracy; B: internal
consistency + cross-doc), briefed identically each loop with no
prior-fix context.

- **Loop 1 (2026-06-12).** C1 (lane B): the § 6 migration INV-map was
  wrong — claimed the format-mismatch test's INV-1/3/4 cover
  append/flip/flip_batch, but its INV-2 actually fuses
  append_batch + create_section. Verified against the test `spec.md` +
  `.cpp`; § 6 rewritten with the real table. C2: INV-3 cited
  `missing_field` (reconciled — see loop 2). M1: annotate edges
  under-specified → § 2.3 expanded. M2: missing edge cases →
  INV-13/14/15 added (multi-Status, batch partial-failure,
  empty-section). M3: INV-1's ANTS-2048 clause lacked § 2 support →
  added. Lane A: `:868` → `:867-870`; canonical-glyph set pinned in
  § 2.3.
- **Loop 2 (2026-06-12).** H1 (lane B): `missing_field` is *undocumented*
  in `mcp-error-codes.md`, not merely drifted — INV-3 switched to the
  documented `bad_args` (one code, both absent + malformed); the
  pre-existing GFM-append drift logged as ANTS-2128. Lane A: the
  `:4170` cite pointed at the `rlErr` lambda, and "parity" was imprecise
  — both resolved by dropping the `missing_field` citation. INV-4/INV-13
  duplication + § 6 fused-INV-2 split clarified; `status` made required
  (was "default when omitted").
- **Loop 3 (2026-06-12).** H1 (both lanes): a loop-2 edit wrongly stated
  the GFM guard "refuses `bad_args`" (it emits `missing_field`) and cited
  the wrong line — corrected to `:4209` + accurate wording. The
  `status`-required contract folded into INV-3; the `4201-4221` range
  clarified to exclude `headline_empty` (`:4213`); the two INV-6 (this
  spec vs the format-mismatch test) disambiguated via a relabelled § 6
  table.
- **Loop 4 (2026-06-12).** Lane A H1: § 2.3's "empty `note` → `bad_args`
  (parity with op:annotate)" contradicted the real guard
  (`:4822` emits `missing_field`) — reframed as a deliberate documented
  choice, annotate guard folded into ANTS-2128 scope. Lane B: H1 —
  `passStatusKeyword` stated as a total 4→4 map with read-only synonyms;
  H2 — `bytes_written` confirmed a real sibling field (`:4675`), kept +
  cited; M2 — INV-13 reworded to own only the first-Status selection
  rule (INV-4 owns byte-identity); bare-emoji cite → `:831-841`.
- **Loop 5 (2026-06-12).** Zero Critical, zero High — convergence.
  Mechanical residue fixed: field-guard range → `4200-4222`; annotate
  cite → `:4822`; INV-15 given § 2.2 design support; INV-16 added (a
  stray `pass` on a GFM roadmap is ignored, not refused); § 2.1 cite →
  `:872-889`; § 7 notes INV-1 is a retained detection assertion; the
  repeated `mcp-error-codes.md:35` cites collapsed to the INV-3
  authority. Loop 5 hit the `--max-loops` cap with only these mechanical
  Mediums; all are fixed in place.
