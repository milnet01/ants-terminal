# ANTS-3504 — ship-date stamp on shipped feedback findings (stale-binary self-check) — review loop log

Record for [`../specs/ANTS-3504-ship-date-stamp-shipped-feedback.md`](../specs/ANTS-3504-ship-date-stamp-shipped-feedback.md) § 5. Cold-eyes loop log.

## Cold-eyes loop log

Two lanes each loop (spec + the `mcp-feedback-files.md` standard), briefed cold.

| Loop | C | H | M | L/I | Notable verified findings (all fixed) |
|---|---|---|---|---|---|
| 1 | 1 | 1 | 4 | 3 | **C1**: `Resolved`-date regex must accept un-parenthesised forms (corpus mixes both) or the feature no-ops on most bullets. **H1**: standard over-claimed the feature as live. Dropped false INV-6 (ANTS-1883 ETag linkage); cited the note convention. |
| 2 | 0 | 1 | 2 | 2 | Named the `ResolveOptions` id→date plumbing; corrected the trimmed-continuation-line claim; reverted the standard's canonical stub to the shipped dateless form with the dated form labelled pending. |
| 3 | 0 | 0 | 3 | 4 | Stated the regex exclusion set accurately (mid-line / "as of" forms are graceful misses); distinguished intra-bullet positional-last from cross-id max; spelt out the `cmdFeedbackQuery` query-side plumbing; added the dated+dateless multi-id test. |
| 4 | 0 | 0 | 0 | 6 | **Converged** — polish only: regex application mode, frozen-first-collapse-date note, minor status-wording alignment. The one standard-lane MEDIUM (doc-wide rule repetition) is pre-existing and already tracked as **ANTS-3479**, not an ANTS-3504 finding. |

Clean at loop 4 (spec lane 0 C/H/M; standard-lane residue is the tracked
ANTS-3479 consolidation, out of scope here).
