# ANTS-1878 — `roadmap_log op:create_section` verb — review loop log

Record for [`../specs/ANTS-1878-roadmap-log-op-create-section.md`](../specs/ANTS-1878-roadmap-log-op-create-section.md) § 8. Cold-eyes loop log.

## Cold-eyes loop log

### Loop 1 (2026-05-26) — joint review with ANTS-1879

Two reviewers, one per spec, briefed against both specs + cited code.

**CRITICAL — fixed.**
- File-ref error: spec pointed at `src/mainwindow.cpp` for the
  `roadmap_log` schema, but the description + schema actually live
  in `src/claudeintegration.cpp:5669+`. Implementer would have
  edited the wrong file. § 7 now cites the correct path.
- Slug transform misdescribed: spec said "non-alphanumeric, trim
  leading/trailing `-`" but `slugifyHeading` uses
  `QChar::isLetterOrNumber()` (Unicode) and only trims trailing `-`
  (leading is suppressed at emission). § 2.2 now reflects the
  actual algorithm with a Unicode example.

**HIGH — fixed.**
- `slug_collision` asymmetry with `uniqueSlug` (which auto-appends
  `-2`) was unstated. § 2.2 now documents the intentional divergence.
- INV-8 carried a conditional "drop this invariant to a manual
  recipe if no seam feasible" wording violating standards.md § 3.5.
  Restated as a single INV-8 — atomicity is the source-grep
  invariant; the commit-failure leg is documented inline as a known
  test-coverage gap (`setForceRoadmapCommitFailForTest` seam not
  yet built; tracked separately).
- `unrecognised_format` gate was described as one condition; it's a
  conjunction (`empty AND > threshold`). § 2.4 + INV-9 now state
  both conjuncts.
- `bad_case` not listed in `mcp-error-codes.md` — spec's § 7
  cross-doc impact now adds it as a catch-up.

**MEDIUM — fixed.**
- `~70 cols` wrap pin was soft; § 2.3 now pins 80 cols on word
  boundaries, matching `roadmap-format.md` § 3.4.
- `line` field semantic was ambiguous (could point at the leading
  blank). § 2.3 dropped the leading blank so the heading is always
  the first emitted line; § 2.6 makes the field semantic explicit.
- Composition with ANTS-1879 didn't state the re-read between
  calls; § 2.6 now states each verb re-parses ROADMAP.md.
- `intro_body` containing a stray heading marker was unspecified —
  added `bad_intro` refusal + INV-10.

**LOW — surfaced (no fix needed).**
- The handler size estimate (§ 4) is qualitative; acceptable for a
  small enhancement spec.

### Loop 2 (2026-05-26) — fresh reviewer, cold re-read

**CRITICAL — fixed.**
- Loop 1 log claimed "Split INV-8 into INV-8a / INV-8b" but the spec
  body actually consolidated back to a single INV-8 (because
  `spec_query`'s bullet-form regex requires `^- \*\*INV-\d+\*\*` —
  letter suffixes break parsing). Log entry rewritten to describe
  what shipped (single INV-8 with the gap documented inline).

**HIGH — fixed.**
- § 6 Tests said "Covers INV-1..INV-9" but the spec has INV-1..INV-10
  (INV-10 = `bad_intro` was added in loop 1). Updated to "INV-1..INV-10".
- § 2.3 contradicted itself: "taken verbatim (no further reformat);
  the implementer pre-wraps". Restated as a single explicit reformat
  policy: paragraph break on single newline, hard-wrap at 80 cols on
  word boundaries, no other transformation.
- Title example was inconsistent across § 2.1, § 2.2, § 2.6 (with vs
  without "Architecture" suffix). Standardised to the no-suffix form
  matching the § 2.6 envelope slug.

**MEDIUM — fixed.**
- INV-7 didn't make the 0-indexed-`lineEnd` vs 1-based-`line`
  conversion explicit; restated as `line == lineEnd + 1`.
- § 7 cross-doc impact missed updating `cmdRoadmapLog`'s
  `bad_op_combo` error string at `:2769-2772` to include
  `create_section`. Added.
- INV-10 / `bad_intro` regex was loose (`^#+ `). Pinned to
  `^#{1,6}\s`, with one positive + one negative test case.
- Deferred `setForceRoadmapCommitFailForTest` seam — kept inline
  note in INV-8 + cold-eyes log; not promoted to ROADMAP because
  the gap is intentional (the existing single-bullet INV-1424
  spec also leaves this leg manual-only). The "if appetite emerges"
  phrasing replaced with a definite "documented gap, not tracked".

### Loop 3 (2026-05-26) — fresh reviewer, cold re-read

**CRITICAL — fixed.**
- § 2.3 cited `roadmap-format.md § 3.4` "wrap body at ~80 cols",
  but § 3.4 is "Theme emojis" and the wrap rule lives in § 3.5
  ("roughly 70 columns"). Restated as a deliberate divergence to
  80 cols (section-intro prose, not bullet body) and removed the
  false citation.
- ANTS-1879 § 2.1 example slug was `phase-106-multi-threading-concurrency`
  but the slugifyHeading transform converts `10.6` (period =
  non-letter-or-number) to `10-6`, producing
  `phase-10-6-multi-threading-concurrency`. Fixed in 1879's
  example to match the canonical transform.

**HIGH — fixed.**
- INV-10 `bad_intro` regex disagreed across § 2.3 (`^#{1,6}\s`),
  § 2.5 (`^#+ `), and INV-10 (`^#{1,6}\s`). Standardised on
  `^#{1,6}\s` everywhere; § 2.5 refusal-codes block updated.
- INV-8 mixed a source-greppable atomicity claim with a Qt
  library-guarantee claim (rollback on commit failure).
  Narrowed INV-8 to the source-greppable "only write path is
  `QSaveFile::commit()`"; moved the
  `setForceRoadmapCommitFailForTest` deferral to § 5 Out of scope
  where it belongs.
- § 7 cross-doc impact mentioned only updating `bad_op_combo`'s
  string but not the dispatch ladder edit needed to actually route
  `op:"create_section"`. Added the explicit "insert
  `if (op == "create_section") return cmdRoadmapLogCreateSection(req);`
  ahead of the `op != "append"` refusal" item.

**MEDIUM — fixed.**
- § 5 Out of scope now explicitly notes stable-prefix support is
  free (no counter touch → no `stable_prefix_unsupported` refusal).
- Loop-2 log entry severity for the INV-8 audit-trail
  inconsistency was overstated as CRITICAL; in retrospect it was
  a MEDIUM (editorial / audit-trail issue, not a contract defect).
  Surfacing here in loop-3 rather than rewriting loop-2's record.

### Loop 4 (2026-05-26) — fresh reviewer, cold re-read (joint pass)

Cross-spec consistency loop reviewing both ANTS-1878 + ANTS-1879
together. No CRITICAL findings. HIGH/MEDIUM polish folded:

**HIGH — fixed.**
- INV-10 (in ANTS-1879) was load-bearing for the dedup story but
  worded "is shared" as if the helper already existed. Restated as
  "implementer MUST extract" so the contract is read as a refactor
  obligation, not as code documentation.

**MEDIUM — fixed.**
- `no_roadmap` refusal was used by `cmdRoadmapLogAppend` (and
  inherited by `create_section`) but missing from
  `mcp-error-codes.md`. § 7 catch-up now adds it alongside
  `bad_case`.
- ANTS-1879 INV-3 lumped three failure modes; expanded to one
  test case per failure code (`headline_empty`, `bad_status`,
  `bad_kind`).

**Loop 4 LOW/INFO surfaced and accepted as known limitations:**
- Stray-heading guard (`^#{1,6}\s`) does not catch Setext
  underlines (`====`) or ATX-closed headings (`# heading #`).
  Acceptable — Markdown rendering of either inside a section
  intro is benign; only the leading-hash form silently extends
  the index.
- LOC estimates in § 4 RAM/build cost are qualitative; per spec
  standard, acceptable for a small enhancement.

The two specs are now considered implementation-ready (spec
draft → ready for sign-off). One more cold-eyes pass is feasible
if further drift is suspected, but loops 1–4 have converged on a
stable surface: every CRITICAL/HIGH finding raised across the
four loops has been fixed and not resurfaced cold.

### Loop 5 (2026-09-27) — review-contract, ANTS-5315 amendment (§ 2.3a)

Gate on commit c427f0b4's amendment (§ 2.3a, § 2.5, INV-2, INV-11). Two
cold `review-lane` lanes, every lane holding every question; both disclosed
arriving with the three CLAUDE.md files, the memory index and the baseline
commit's subject. **Q1 1 · Q2 1 · Q3 2 · Q4 0** — verified 4, fixed 4,
dismissed 0. The duplicate-version refusal now fires placed or supplied and
against `##` and `###` titles alike (found by both lanes); `v`-prefixed and
pre-release versions refuse `bad_title`; `placed_by_version` is absent on a
supplied `after_section`. One open question resolved clean (placement in an
unsorted or range-titled roadmap: "greatest lower version" is defined in any
order). One pre-existing finding outside the gated span, the wrong files in
§ 2.1, § 6 and § 7, was filed as ANTS-5496 rather than fixed here.

### Loop 6 (2026-09-27) — review-contract, ANTS-5315 amendment, at the cap

Second and last loop of this run (a spec's cap is 2). Two cold lanes, every
lane holding every question, same disclosures as loop 5 plus loop 5's
commit subject. **Q1 0 · Q2 1 · Q3 2 · Q4 0** — verified 3, fixed 3,
dismissed 0. § 2.3a's version checks now run before derivation and before
the collision check, so `bad_title` wins where `missing_field` or
`slug_collision` would also apply (both lanes; INV-6 names the exception);
a pre-release suffix is a `-` directly after the version; "absent" reads
"absent or empty". Three open questions resolved clean or out of scope
(document order, `0.10.00`, a numbered patch beside a patch stream).
**Cap: calm.** Two of the final loop's three findings landed on loop 5's
own text, and both were consequences it had not propagated rather than
repairs of repairs. All seven verified findings of the run fell inside the
armed span, c427f0b4. The document runs to about 540 lines. Shipped to
implementation; ANTS-5496 carries the one pre-existing finding.

### impl (2026-11-14, ANTS-5777)

Not a review; no reviewer was dispatched. § 2.5's `bad_title` line now says the duplicate-version check also covers a section in an archive file, on the store route.
