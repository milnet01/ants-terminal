# ANTS-1436 — `roadmap_query` pagination + auto-truncate for large active payloads — review loop log

Record for [`../specs/ANTS-1436-roadmap-query-pagination-auto-truncate.md`](../specs/ANTS-1436-roadmap-query-pagination-auto-truncate.md) § Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1** (2026-10-12, ANTS-5651 amendment; review-contract, 2 cold lanes,
  every lane held every question) — Q1 5 · Q2 6 · Q3 0 · Q4 0 (verified 11 /
  dismissed 0; fixed 11). In the amendment: the effective-limit paragraph named
  a byte budget INV-9 did not (now points at INV-9); the Status line said
  nothing of the unbuilt amendment; INV-4's heading said `limit` where the body
  and the new cut need the rows returned. Records of shipped code, fixed in
  the run under rule 14's code-exists exception: step 2 and INV-3 use
  `softCapBytes() - kEnvelopeOverheadBytes` and keep at least one row
  (ANTS-5103), with `next_offset: offset + n`; INV-1's detection names
  `shouldEmitPaginationFields`; INV-6 and its design and test lines describe
  section_index paging (ANTS-1729); `limit < 1` is `bad_args`; INV-10 names
  `remotecontrol_roadmap_query_verb.cpp`; INV-11 gives the real signature and
  five call sites (from an open question; the test asserts five). Both lanes
  raised most of these.
- **Loop 2** (2026-10-12, ANTS-5651 amendment; 2 cold lanes, every lane held
  every question; cap reached) — Q1 2 · Q2 1 · Q3 0 · Q4 1 (verified 4 /
  dismissed 0; fixed 4). Fixed: step 3 and INV-3 measure `filtered.mid(offset,
  n)`, as the code slices from `offset` (both lanes); the Tests section names
  the two INV-9 tests the cut rewrites; INV-9's `limit` echo is scoped to
  envelopes that echo it (from an open question: `changelog_query` and
  section_index carry no such field); INV-8 drops a "64-byte verbatim
  hygiene" the handler does not apply. Cap reached (2 for a spec): calm, 1 of
  this loop's 4 findings landed on text loop 1 wrote. Whole run: 4 of 15
  verified findings fall inside the gated span, commit 6093600f; the rest were
  stale records of shipped code. This loop's 4 fixes were read by no lane;
  the build reads them. Tail filed on ANTS-5651: whether § Dispatch
  forwarding's "only dispatch lambda that forwards verbatim" still holds.
- **amend (2026-11-13, ANTS-5666)** — not a review; no reviewer dispatched.
  § Dispatch forwarding no longer says one `mainwindow.cpp` lambda is the only
  verbatim forwarder: `rcDelegate` forwards the whole `args` object
  (ANTS-3422). The per-argument forwarding snippet is gone with it.
