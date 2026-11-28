# roadmap_log_review_fixes — the smaller roadmap_query / roadmap_log fixes

Contract for ANTS-6641: the rows of
`docs/reviews/close-findings-2026-11-06.md` queued under it that changed
behaviour. Each case drives the verb through `cmdRoadmapLog` or
`cmdRoadmapQueryForTest` with `XDG_DATA_HOME` in the case's own temporary
folder, so no case reads or writes the machine's roadmap store. A store case
migrates its fixture into that sandboxed store first.

## Invariants

- **INV-1** (row 97) — a `query` under `regex:true`, or a `body_match` under
  `body_match_regex:true`, that nests a quantifier or an alternation under a
  quantifier refuses `bad_args`. Case `Row97NestedQuantifierRefuses`.
- **INV-2** (rows 97, 107) — `body_match` is capped at 200 characters and its
  control bytes become `?`, in the match and in its echo. Case
  `Row107BodyMatchEchoIsCappedAndScrubbed`.
- **INV-3** (row 98) — `amend_headline` refuses a `new_text` holding a line
  break, `bad_args`, on the store and on the file. Case
  `Row98HeadlineNewTextLineBreakRefuses`.
- **INV-4** (row 101) — a pass-headings file append refuses a designator the
  file already holds, `id_taken`; `append_batch` skips it with that code; a
  dry run names the id `would_be_id`, not `id`. Case
  `Row101PassFileAppendRefusesATakenDesignator`.
- **INV-5** (row 102) — a `lanes` element is sanitised as an `evidence` one
  is, on append and on `amend_field`: a newline or comma folds to a space.
  Case `Row102LanesAreSanitised`.
- **INV-6** (row 104) — `amend_batch` reports `note_scrubbed_params` on each
  amended row, `replace_text` once per call. Case
  `Row104BatchBodyEditsReportTheScrub`.
- **INV-7** (row 105) — on the store path a note counts as already present
  only when its lines stand in the body as whole lines. Case
  `Row105ShortNoteInsideALineIsStillAppended`.
- **INV-8** (row 106) — a store flip to the item's current status leaves
  `last_modified` and the history table as they were. Case
  `Row106SameStatusFlipTouchesNothing`.
- **INV-9** (row 108) — a plan whose apply fails `write_failed` is used up: the
  store committed, so the same `plan_id` then refuses `plan_not_found`. Case
  `Row108WriteFailedDoesNotRestoreThePlan`.
- **INV-10** (row 109) — a store-served pass flip by `id` reaches an item the
  file has lost. Case `Row109StorePassFlipLocatesByIdInTheStore`.
- **INV-11** (row 111) — `amend_field field:"section"` refuses more than 500
  `locators`, `bad_args`. Case `Row111SectionMoveCapsLocators`.
- **INV-12** (rows 1381, 1382) — a file-path flip's dry run carries
  `note_scrubbed_params` and, under `return:"headline_only"`, `post_bullets`;
  the GFM reply carries `format:"gfm"`. Case
  `Row1381FilePathFlipDryRunCarriesTheEcho`.
- **INV-13** (row 1384) — `amend_body`'s file-path `bullet_not_found` carries
  `suggestions`. Case `Row1384AmendBodyFileMissSuggests`.
- **INV-14** (row 1387) — an `amend_field_batch` entry's refusal names
  `amend_field_batch`. Case `Row1387BatchEntryRefusalNamesTheBatchOp`.
- **INV-15** (row 1390, ANTS-5923) — a caller in a subdirectory of an
  unmigrated project reads the root's declared id format, on the list path and
  the section path. Case `Row1390SubdirectoryCallerReadsTheRootDeclaration`.
