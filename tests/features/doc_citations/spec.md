# doc_citations — engine: resolve a doc's citations and read the cited text (ANTS-3636)

Contract: [`docs/specs/ANTS-3636.md`](../../../docs/specs/ANTS-3636.md). This
file is the test-surface map; the spec owns the ladder, the statuses and the
caps.

`src/doccitations.{h,cpp}` — `DocCitations::check(rootCanonical, docAbsPath,
opts)` takes a document, resolves every citation `DocCitations::scan`
(ANTS-3653) found in it, reads each target, and returns the § 2.1 envelope. It
writes nothing and holds no cross-call state. Qt6::Core-only, in
`ants_core_lib`, so these fixtures need a `QTemporaryDir` and a seeded
`basenameIndex` — never a `CodebaseIndex` on disk.

The verb layer (`docCitationsValidate`, `docCitationsClampOptions`, the six
registration hooks) is `tests/features/doc_citations_verb/`, in the `test_claude`
bundle. Two directories because the two layers live in two bundles.

## Not covered here

**The anchor-symbol check is ANTS-3654.** Until it ships, `anchor_symbol` and
`anchor_found` are never emitted, `counts.anchor_missing` is always 0 and every
`ok` citation is `counts.unchecked`. `only:"stale"` therefore selects exactly the
non-`ok` citations. Three invariants are half-covered as a result and say so at
their test: INV-23 (the `\r` strip is asserted on emitted text, not on the anchor
comparison), INV-26 (the identity holds with the anchored-and-found term at 0),
INV-28 (`anchor_found:false` present-when-checked lands with ANTS-3654).

The citation grammar itself is `tests/features/doc_citations_scan/`; the
`MarkdownScan` primitives are `tests/features/markdownscan/`.

## Contract

- **INV-4** — a bare basename matching exactly one indexed path resolves; ≥2 is
  `ambiguous` with no `text`/`path`, `candidates[]` sorted by UTF-16 code unit
  and capped at `maxCandidates`, `candidates_total` the true count.
  *Test:* `DocCitations.Inv4BasenameIndexUniqueAmbiguousAndCapped`.
- **INV-5** — a bare basename absent from the index but present at the repo root
  resolves (step 3); absent from both → `unparsed[]`/`unresolvable`.
  *Test:* `DocCitations.Inv5RepoRootFallbackAndUnresolvable`.
- **INV-6** — a lexically out-of-root citation is rejected at step 0 with **zero
  filesystem calls**. *Test:* `DocCitations.Inv6OutOfRootRejectedWithoutStat`.
- **INV-7** — a continuation inherits the antecedent's resolved path only, carries
  `inherited_path:true`, and computes its own status.
  *Test:* `DocCitations.Inv7ContinuationInheritsPathOnly`.
- **INV-8** — a fenced block resets the antecedent tracker.
  *Test:* `DocCitations.Inv8FenceResetsAntecedent`.
- **INV-11** — `missing_file` / `out_of_range` / `read_error`, none emitting
  `text`. *Test:* `DocCitations.Inv11StatusesWithoutText`.
- **INV-12** — `max_range_lines` truncates the emitted lines, `end_line` unchanged.
  *Test:* `DocCitations.Inv12RangeTruncated`.
- **INV-15** — a shape-matching token resolving nowhere is `unparsed[]`, `raw` the
  grammar's capture. *Test:* `DocCitations.Inv15UnresolvableRawAndClip`.
- **INV-16** — `check` writes nothing and holds no cross-call state.
  *Test:* `DocCitations.Inv16NoWritesNoState`.
- **INV-20** — the line cache reads each cached target at most once; past the
  bounds a target is re-read per citation **with correct text both times**.
  *Test:* `DocCitations.Inv20LineCacheOpenCounts`.
- **INV-23** — a trailing `\r` is stripped from emitted `text`.
  *Test:* `DocCitations.Inv23CarriageReturnStripped`.
- **INV-25** — gate G: a candidate whose canonical path leaves the root, including
  via a symlinked directory component, is never opened.
  *Test:* `DocCitations.Inv25GateGCanonicalises`.
- **INV-27** — a range past EOF is `ok` + `end_clamped`; `range_truncated` is not
  set by that alone; all three range flags may fire together.
  *Test:* `DocCitations.Inv27EndClampedIndependentOfTruncation`.
- **INV-30** — `doc_lines` is true length, `scanned_lines` the scanned prefix.
  *Test:* `DocCitations.Inv30DocLinesVersusScannedLines`.
- **INV-34** — a directory-bearing citation naming a directory is `missing_file`;
  a bare one is `unresolvable`; step 2's else is `missing_file`.
  *Test:* `DocCitations.Inv34DirectoryAndStaleIndexAsymmetry`.
- **INV-37** — `basename_index_truncated` comes from `Options`, never from the
  index's size. *Test:* `DocCitations.Inv37IndexTruncatedFlagIsPassedIn`.
- **INV-41** — `file_lines` is emitted for exactly `ok` and `out_of_range`.
  *Test:* `DocCitations.Inv41FileLinesOnlyForOkAndOutOfRange`.
- **INV-43** — each emitted line is capped at `maxTextBytes` UTF-8 bytes, cut on a
  character boundary. *Test:* `DocCitations.Inv43TextClippedAtUtf8Boundary`.
- **INV-45** — a citation whose text contains a token `wrapMcpData` rewrites
  carries `text_escaped:true`. *Test:* `DocCitations.Inv45TextEscapedDisclosed`.
- **INV-46** — past `maxTargetReads` a citation needing a fresh read is
  `read_error` and `read_budget_exhausted` is set.
  *Test:* `DocCitations.Inv46ReadBudgetExhausted`.
- **INV-47** — the field set, for an existing doc and for a well-formed
  non-existent one; plus the one refusal `check` itself raises.
  *Test:* `DocCitations.Inv47FieldSetAndEngineRefusal`.
- **INV-17** — the two resumable caps drop whole trailing entries and emit
  `next_offset`; `maxUnparsed` does not.
  *Test:* `DocCitations.Inv17ResumableAndNonResumableCaps`.
- **INV-26** — the three tally identities hold with every bucket non-zero.
  *Test:* `DocCitations.Inv26TallyIdentities`.
- **INV-28** — every omit-when-false flag is omitted; `ok`/`truncated` always
  emitted; `unterminated_fence` absent rather than 0.
  *Test:* `DocCitations.Inv28FlagsOmittedWhenFalse`.
- **INV-31** — `max_doc_lines` is not resumable by paging, but a resumable cap
  binding inside the scanned prefix still emits `next_offset`.
  *Test:* `DocCitations.Inv31DocLineCapNotResumable`.
- **INV-35** — at the `max_bytes` floor at least one entry is emitted and
  `next_offset` strictly advances.
  *Test:* `DocCitations.Inv35AtLeastOneEntryAtTheFloor`.
- **INV-38** — `offset` at or past the post-filter length returns empty with no
  `next_offset`. *Test:* `DocCitations.Inv38OffsetPastTheEnd`.
- **INV-42** — `count` and `counts` are whole-doc even when an emission cap binds.
  *Test:* `DocCitations.Inv42CountsAreWholeDoc`.

## Trap cases

| INV | What passes without the fixture |
|---|---|
| 6 | the grammar needs a dot in the final segment, so a dotless out-of-root path never matches and step 0's leading-`/` branch goes unexercised while the test passes |
| 11 | under a root CI container the permission bits are ignored, so the mode-000 row cannot fail — hence the `geteuid() == 0` skip guard |
| 26 | a partition tested with empty buckets, which cannot catch a miscount into `ok` |
| 27 | an implementation treating `end_clamped` and `range_truncated` as mutually exclusive |
| 28 | an implementation applying omit-when-false uniformly, deleting the verb's primary signal |
| 34 | an implementation that gave ladder step 3 no else — unreachable, so never caught |
| 35 | an implementation treating `max_bytes` as a hard cap |

## Test

`tests/features/doc_citations/test_doc_citations.cpp`, compiled into the
`test_core` bundle (Core-only) per `tests/features/README.md` — not a standalone
`add_executable`. Verified RED against feature-absent code before the
implementation landed.
