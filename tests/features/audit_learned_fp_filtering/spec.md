# Feature: a learned false positive is actually hidden

**Status:** shipped (ANTS-4444)

Marking a finding as a false positive writes a line-independent content
fingerprint to the learned-FP ledger, and
`AuditEngine::applyLearnedFpSuppressions` sets `suppressed` on any finding
matching one.

Every render and export path filtered on `isSuppressed(f.dedupKey)` instead,
which consults the suppressed-**key** set and never the fingerprint set. So
a learned false positive was still listed in the results pane, counted in
the summary, written to HTML and SARIF, offered to the ROADMAP fold, and —
because the engine leaves `aiVerdict` empty — re-sent to the AI on every
triage click.

Two places *did* honour the cached flag (auto-fix and the quality tracker),
so the two halves of the system disagreed about what was suppressed.

The fix keeps the deliberate **live-lookup** design that SARIF export
documents: a suppression added mid-session must take effect without
re-running the audit, which a flag cached at parse time cannot express. So
the fingerprint set is consulted live too, rather than the paths switching
to `f.suppressed`.

## Invariants

**INV-1 — the Finding overload consults the learned-FP ledger.**
Source-grep against `src/auditdialog.cpp`: `isSuppressed(const Finding &)`
must reference `m_learnedFpFingerprints` and `computeFingerprint`.

**INV-2 — it delegates to the key lookup rather than duplicating it.**
Source-grep: its body must call `isSuppressed(f.dedupKey)`, so the legacy
16-character prefix match keeps working for learned findings too, and there
is one definition of "suppressed by key".

**INV-3 — every render and export filter takes the whole Finding.**
Source-grep: no `if (isSuppressed(` may be passed a bare `dedupKey`. That
spelling is what made the ledger invisible. Scoped to exclude the Finding
overload's own body, where INV-2 requires exactly that spelling as the
delegation — unscoped, INV-2 and INV-3 contradict each other and one must
always fail.

**INV-4 — the parse-time cache still primes from the key set.**
Source-grep: the `f.suppressed = isSuppressed(...)` assignment must keep the
`dedupKey` form. It runs *before* `applyLearnedFpSuppressions`, which is
what sets the flag for learned findings; routing it through the Finding
overload would compute a fingerprint for nothing. This invariant exists
because a bulk edit of INV-3's spelling silently rewrites this line too.

**INV-5 — the empty ledger costs nothing.** Source-grep: the overload must
return before hashing when the fingerprint set is empty. The filters run per
finding on every render, and the fingerprint is a SHA-256.
