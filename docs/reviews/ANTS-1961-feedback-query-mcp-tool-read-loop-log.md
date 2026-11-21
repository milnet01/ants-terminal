# ANTS-1961 — `feedback_query` MCP tool (read the un-triaged tail of a `*_Ants_MCP_Feedback.md` file) — review loop log

Record for [`../specs/ANTS-1961-feedback-query-mcp-tool-read.md`](../specs/ANTS-1961-feedback-query-mcp-tool-read.md) § 8. Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-06-03)** — 3 cold reviewers (read-verb lane, write-verb
  lane, cross-cutting lane). Verified + fixed: byte-cap default 256→512 KiB
  ("mirrors read_log" was contradictory; `readlog.h:18` is 512); missing
  `path` refusal collapsed to a single code `bad_args` (was
  `bad_args/missing_field`); `###`-inert clause pinned in INV-5; brittle
  `:2888` line literal dropped for region phrasing; explicit
  `callerCwdContractFor` branch added to Surface (ANTS-1417 build
  coverage); `allowOutsideRoot` 5-arg signature cited to
  `pathvalidation.h:46-50`; suffix guard noted as tool-side (standard has
  only the glob); Status token → `spec draft`; added § 4 RAM/build cost,
  § 7 Cross-doc impact, and `*Test:*` clauses on every INV. The four
  off-taxonomy codes (`missing_field`/`bad_op`/`bad_id`) were remapped to
  canonical `bad_args`/`bad_mode`; `not_feedback_file` was added to
  `mcp-error-codes.md` as a justified new row.
- **Loop 2 (2026-06-03)** — cold re-read, 3 lanes. Verified + fixed:
  `delta_start_line` pinned to the first contributor-heading line (struct
  comment + INV-8); INV-5 now states the append-at-end precondition that
  makes the single-step framing equal the standard's two-step algorithm;
  INV-6 split into the two distinct `delta_present` branches; § 4 RAM
  corrected — `feedback_query` reads the whole file (peak = file size, not
  `max_bytes`); ETag opt-in model re-pointed to `roadmap_query`/
  `current_state` (read_log is not ETag-enabled); suffix guard promoted to
  a canonical home in `mcp-feedback-files.md` (both verbs now cite it);
  `feedbackfile.h` declared the full shared `FeedbackFile` surface; anchor
  regex canonical home named. Dismissed (unverified): a claimed off-by-one
  in the `claudeintegration.cpp:7959-7961` cite (grep confirms 7959 is the
  comment start) and a 1417-vs-1419 mechanism conflation (the spec cites
  the ANTS-1417 coverage test, faithful to the source comment).
- **Loop 3 (2026-06-03)** — cold re-read, 3 lanes, 0 CRITICAL. Findings
  were all line-cite drift caused by loop 2's own insertion of the
  suffix-guard paragraph into `mcp-feedback-files.md` (shifting line
  numbers ~+8). Fixed by switching the standard cites to durable
  section-name references (append-at-end precondition → § "The un-triaged
  delta"); T9 given explicit skip-when-absent semantics; the ~150 KB
  corpus figure confirmed by `wc -c` (152,549 bytes). Convergence loop.
- **Loop 4 (2026-06-03)** — cold re-read, 3 lanes: **0 CRITICAL, 0 HIGH**
  (clean pass). Remaining MEDIUM/LOW polish folded: named the explicit
  `isEtagSupportedTool()` `||`-chain edit in § 2.5 (the one ETag wiring
  step the Surface left implicit); padded the § "The un-triaged delta"
  cite to the full "(parser contract)" heading. The off-by-one
  `:7959-7961` nit re-raised and re-dismissed (7959 is the comment-region
  start). Loop closed.

### ANTS-3366 / 3371 / 3376 / 3384 amendment review (2026-06-30)

A fresh 4-loop cold-eyes pass covering the post-acceptance amendments
(`include_tracking` INV-12; the not_found candidate list INV-13; default
path derivation + `path_derived` INV-14; the canonical-basename / DOOM
leaf-mismatch rule in the standard). Run alongside ANTS-1962.

- **Loop 1** — 3 lanes. **1 CRITICAL** (the standard's legacy-alias example
  cited a non-existent `Album_Builder/` dir; the real leaf-mismatch case is
  DOOM at `DOOM_Ants/`), plus the `caller_cwd_required`-vs-`bad_args`
  refusal overlap (INV-3/INV-4 reconciled), the shipped ANTS-3366 sibling
  test given a T-number (T14), and an INV-8 not_found field inventory.
- **Loop 2** — 3 lanes, 0 CRITICAL. HIGH staleness: corpus figure
  "~2,500 lines/150 KB" → measured 3,460 lines/~240 KB; the 6-entry brand
  sample → a complete 8-row dir-leaf↔filename table; the DOOM not_found
  recovery corrected (its real file is NOT auto-surfaced — it hits
  `all_other_projects:true`, so explicit `path` is mandatory). INV-14
  `all_other_projects` test attribution fixed (asserted in T12, not T13).
- **Loop 3** — 3 lanes, 0 CRITICAL. 1 HIGH was a loop-2 self-inflicted
  not_found example that glued two branches together (split into the
  own-match example + the leaf-mismatch description); 2 HIGH were
  pre-existing line-cite drift (`claudeintegration.cpp:7959-7961` →
  `callerCwdContractFor`; `remotecontrol_roadmap_query.cpp` → `g_forceCounterCommitFail`)
  caused by later file growth — converted to durable symbol cites. Added
  the §2.3 not_found example envelope + candidates ordering.
- **Loop 4** — 3 lanes: **0 CRITICAL, 0 HIGH on the feature surface.** All
  HIGHs were pre-existing (a marker-comment ambiguity in the standard; an
  INV-10/T1 etag-round-trip test the feature `spec.md` marks out of scope;
  T1 conflating two gtests). Folded: documented `fbNotFound`'s third
  (no-leaf) hint branch; corrected the INV-10/T1 etag claim to dispatch-
  layer; named the two gtests behind T1; INV-13 ordering wording. Deferred
  (pre-existing, surfaced not auto-fixed): a full T1→T1a/T1b renumber of
  this spec's original test-ID scheme. Convergence: the ANTS-3376/3384
  contract verified accurate against the shipped code in every loop.
