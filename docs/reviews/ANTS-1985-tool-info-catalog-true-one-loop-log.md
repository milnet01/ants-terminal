# ANTS-1985 — `tool_info {catalog:true}`: one-call full-verb catalog — review loop log

Record for [`../specs/ANTS-1985-tool-info-catalog-true-one.md`](../specs/ANTS-1985-tool-info-catalog-true-one.md) § Cold-eyes loop log.

## Cold-eyes loop log

Numbering note: loops 1–4 predate the Loop-5 renumber, so where they
say "INV-4" / "INV-6" they mean this doc's own pre-renumber ids — now
**INV-12** / **INV-14** respectively. `mcp_orientation_install INV-10` /
`INV-12` always mean that orientation test's own ids.

- **Loop 1 (2026-06-04).** Findings folded: handler must evaluate the
  catalog branch *before* the `reqName.isEmpty()` → `missing_name`
  guard (added § 2.4 + INV pseudocode); softened the unmeasured byte-cap
  claims and attributed the "~24 B headroom" figure to the ROADMAP
  source (§ 2.6, INV-6); corrected the illustrative envelope counts
  (`category_count` ~22 + `other`, not 21 — § 2.1); brought the spec
  into `specs.md` shape (numbered sections, `Surface` heading, per-INV
  `*Test:*` clauses, dated `spec draft` Status, this log). Dismissed:
  `**Lanes:**` header line flagged as non-standard — kept as established
  corpus convention, harmless to `spec_query`.
- **Loop 2 (2026-06-04).** Accuracy nits folded: corrected the
  `kindForName` line range to `7245-7345` (§ 2.1); named the exact
  `required`-array construction site `6959-6961` as the schema edit
  point (§ 2.5); time-stamped the "~24 B headroom" figure as a
  spec-time reading (§ 1). No CRITICAL/HIGH; all load-bearing claims
  (handler dispatch order, category derivation, snapshot timing, byte
  cap) verified sound against live source.
- **Loop 3 (2026-06-04).** Folded: pinned the prelude byte budget with
  measured figures (current block 179 B, reference replacement 134 B,
  headroom widens 45 B — § 2.6); restated this doc's prelude invariant
  so `mcp_orientation_install INV-12` is described as a verbatim-install
  check, not a byte-cap guard (only that test's INV-10 enforces the
  cap); made the category parse total by specifying the
  malformed-bracket (`[` unclosed / empty `[]`) fallback to `"other"`
  (§ 2.2, INV-4). All cited line numbers / symbols re-verified exact.
- **Loop 4 (2026-06-04).** Folded: reconciled the `category_count`
  example with the prose — `other` is counted only when present, never
  a fixed `+1` (§ 2.1); softened "single source of truth" since the
  catalog reads both the category prefix and `selection_hint` (§ 2.2);
  clarified the 134 B figure is a raw-string measurement with the
  rendered cap enforced by INV-10, not the byte arithmetic (§ 2.6). No
  CRITICAL/HIGH across loops 2–4; all measured figures (179 B / 134 B)
  and line citations confirmed exact by the reviewer.
- **Loop 5 (2026-06-04).** Folded one HIGH: the design-doc invariants
  collided in number with ANTS-1399's `INV-1..8` in the shared
  `mcp_tool_info_verb` test home — renumbered to **INV-9..INV-14** per
  `specs.md` §5.5 and disambiguated every prose `INV-N` reference as
  ANTS-1399 vs ANTS-1985 (§ 2.7, INV-13, § 6). Clarified the 179/134 B
  figures as the heredoc representation (lines incl. `\n`). No
  CRITICAL; all citations re-confirmed exact.
- **Loop 6 (2026-06-04).** Editorial only: disambiguated bare `INV-N`
  ids in this log (numbering note + qualified the loop-3 entry);
  quoted the current "Full catalog:" block verbatim in § 2.6 so it can
  be diffed against HEAD; rephrased INV-14's byte-cap-vs-byte-equality
  double negative. No CRITICAL/HIGH; the design contract has been
  stable and verified clean since loop 1.
