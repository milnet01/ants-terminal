# DebtSweepEngine — Pure-Helper Conformance

Locks the public API of `src/debtsweepengine.{h,cpp}` introduced in
ANTS-1113 v1 and expanded in ANTS-1358. See `docs/specs/ANTS-1113.md`
for the v1 design and `docs/specs/ANTS-1358.md` for the
detector-expansion addendum.

## Invariants under test

- **INV-3.** `detectOrphanQUnused` returns a Finding for
  `Q_UNUSED(stale)` in a file that does not declare `stale`.
  Returns empty when the same file declares `int stale = 0;`.
- **INV-4.** `detectMissingInvariantTests` returns a Finding for
  a feature dir whose `spec.md` declares `INV-7` and whose
  `test_*.cpp` does not mention `INV-7`. Returns empty when the
  test file mentions it. Alphanumeric IDs work too: `INV-8b`
  produces a Finding when the test file omits it.
- **INV-9.** `applyMechanicalFix` deletes the marker line for an
  `orphan_q_unused` Finding; the post-fix file has one fewer
  line and no longer contains the marker on the original line
  number; the verdict is `{applied:true, errorCode:"",
  errorMessage:""}`. Non-fixable Finding → `{applied:false,
  errorCode:"not_fixable", …}` and file unchanged.
- **INV-10.** `templateDebtSweepFoldInBlock` first line is
  `### 🧹 Debt-sweep fold-in (<dateIso>)`; first bullet starts
  with `- 📋 [ANTS-<id>]`; bullet contains `Kind: chore.` and
  `Source: debt-sweep-<dateIso>.`; the `<dateIso>` token is
  byte-identical between heading and Source.
- **INV-11.** `triagePrompt` emits a prompt containing one
  `[<category> / <detector_id>] <file>:<line>` block per input
  Finding.
- **INV-13a.** `applyMechanicalFix` returns
  `{applied:false, errorCode:"file_changed", …}` when the marker
  is no longer on the cited line (idempotent re-apply guard).
- **INV-13b.** `templateDebtSweepFoldInBlock` returns empty when
  inputs are mismatched-length or `deferred` is empty.

## ANTS-1358 detector expansion

- **INV-14.** `detail::scanDuplicateIncludes` returns one
  auto-fixable Finding (line = second occurrence) for a file that
  `#include`s the same header twice; empty when each header appears
  once; an include duplicated across `#if`/`#endif` branches is not
  flagged.
- **INV-15.** `applyMechanicalFix` deletes the redundant line for a
  `duplicate_include` Finding (`applied:true`); returns `file_changed`
  when the cited line is not a still-duplicated include.
- **INV-16.** `detail::scanObsoleteQStringIdioms` returns
  auto-fixable Findings for `QString::null`, `toAscii`, `fromAscii`;
  empty for `QString()` / `toLatin1` / `fromLatin1`.
- **INV-17.** `applyMechanicalFix` rewrites the obsolete idiom in
  place (`QString::null` → `QString()`) for an `obsolete_qstring_idiom`
  Finding (`applied:true`); a second apply returns `file_changed`.
- **INV-18.** `detail::scanDeadBranchAfterReturn` flags a
  non-auto-fixable Finding for a statement after a bare `return …;`;
  empty when the following line is `}` / `case` / a label / `#…`, and
  when the `return` is conditional (`if (x) return;`).
- **INV-19.** `detectStaleTodos` (git-backed) flags a TODO whose blame
  committer-time predates the threshold, not auto-fixable; returns
  empty when `staleTodoMaxAgeDays <= 0`.

## ANTS-3346 bulk-defer triage gate

- **INV-20.** `evaluateTriageGate(deferred, triaged)` is a pure
  predicate over the deferred set:
  - Gates on TOTAL deferred count, not composition — auto-fixability is
    not a safety signal, so a large all-auto-fixable batch is gated too
    (the mostly-`[fix]` 708-finding "Defer all" case).
  - Exactly `kBulkDeferTriageThreshold` findings is allowed (refusal is
    strictly greater-than); one more is refused (`allowed:false`,
    non-empty `reason`).
  - `triaged:true` always allows, regardless of size.
  - Echoes `total`, `nonAutoFixable` (message only), `threshold` for the
    caller's refusal envelope.
