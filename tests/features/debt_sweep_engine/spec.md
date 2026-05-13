# DebtSweepEngine — Pure-Helper Conformance

Locks the public API of `src/debtsweepengine.{h,cpp}` introduced in
ANTS-1113 v1. See `docs/specs/ANTS-1113.md` for the full design.

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
