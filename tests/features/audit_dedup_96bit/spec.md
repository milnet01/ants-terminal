# Feature: widen `computeDedup` from 64 to 96 bits

## Contract

`auditdialog.cpp::computeDedup` MUST emit a hex digest at least 24 chars
long (96 bits of SHA-256 prefix). The pre-0.7.29 width (16 hex chars,
64 bits) was vulnerable to birthday collisions once a project's
finding inventory crossed roughly the 4-billion-key mark — well beyond
any single project's lifetime, but uncomfortable when the dedup key is
also the SARIF `partialFingerprints.primaryLocationLineHash`,
the `result.suppressions[]` lookup key, the URL-fragment of the
"Suppress this finding" anchor, and the JSONL persistence key.
Widening to 96 bits buys 16 million× more headroom for the same memory
footprint (an extra 8 bytes per stored key).

The migration MUST stay transparent for users with pre-0.7.29 v1/v2
JSONL suppression files. Existing 16-char keys keep working: when a
new 24-char finding's dedup key is checked, the lookup helper also
accepts a match on the leading 16 chars.

## Rationale

Dedup keys appear in:
- `Finding::dedupKey` — used in suppression JSONL, SARIF
  `partialFingerprints`, the HTML report's "anchor", and the rule-quality
  tracker's per-rule history.
- `m_suppressedKeys` — `QSet<QString>` membership test on every render.
- `RuleQualityTracker` — keyed by dedup key in the per-rule history.

A 64-bit truncated digest collides at ~2³² entries with 50 % probability.
Mid-life projects with hundreds of accumulated suppressions and
thousands of fingerprinted findings stay comfortably below that, but
the collision-cost is high (a wrong-finding suppression). 96 bits raises
the collision threshold to ~2⁴⁸, which is well past any plausible
project's lifetime collection.

The change is one-line in `computeDedup` (`.left(16)` → `.left(24)`)
plus a backward-compatibility helper that lets existing 16-char keys in
`~/.audit_suppress` still match new 24-char findings.

## Invariants

**INV-1 — `computeDedup` emits ≥ 24 hex chars.** Source-grep against
`src/auditdialog.cpp`: the body of `computeDedup` must contain
`.left(24)` (or a wider literal) inside the `QCryptographicHash::hash`
chain. The pre-fix `.left(16)` MUST NOT appear in `computeDedup`'s
body.

**INV-2 — A backward-compatibility lookup helper exists.** Source-grep
against `src/auditdialog.h` or `src/auditdialog.cpp`: a member
function whose body queries `m_suppressedKeys` AND extracts a 16-char
prefix (`.left(16)` substring lookup) of an input key — i.e. the
helper accepts both a full 24-char match and the legacy 16-char prefix
match. The helper name should reflect the role (`isSuppressed`,
`hasSuppression`, etc.).

**INV-3 — Render-pipeline call sites use the helper, not raw `contains`.**
Source-grep against `src/auditdialog.cpp`: at least four call sites
that previously read `m_suppressedKeys.contains(f.dedupKey)` MUST now
go through the helper (so the legacy-prefix path applies). We allow
two raw-`contains` sites to remain (loadSuppressions self-population
and saveSuppression de-duplication on insert), since those operate on
exact stored keys, not on potentially-newer 24-char `f.dedupKey`s.

**INV-4 — saveSuppression mirrors the new key into the reasons map.**
Source-grep: the `saveSuppression` function body must update a
`m_suppressionReasons` (or similarly-named) map alongside the
`m_suppressedKeys.insert` call. This is the substrate the SARIF
`suppressions[].justification` field reads — without it the SARIF
export can't surface why a finding was suppressed.

## Scope

### In scope
- `computeDedup` width increase.
- `isSuppressed` helper for backward-compat lookup.
- Transparent rewrite of seven `m_suppressedKeys.contains(f.dedupKey)`
  call sites to use the helper.
- Reasons map populated alongside the keys set.

### Out of scope
- Migrating existing 16-char keys to 24-char on load. Out of scope —
  matching by prefix preserves behaviour without rewriting user state.
- Recomputing rule-quality tracker keys. The tracker is keyed by raw
  dedup-key and starts collecting fresh entries; old entries become
  inert and age out via the 30-day rolling window.

## Regression history

- **0.6.x – 0.7.28:** `.left(16)` fixed-width 64-bit truncation.
  Collision threshold ~4 billion entries; comfortable but tight when
  the same key is the SARIF fingerprint, suppression key, anchor URL,
  and rule-quality bucket.
- **0.7.29 (this fix):** widen to 24 hex chars, add prefix-matching
  helper, populate suppression-reasons map for the SARIF
  `suppressions[]` work in this same release.
