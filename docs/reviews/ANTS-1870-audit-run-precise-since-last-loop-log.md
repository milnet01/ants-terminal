# ANTS-1870 — `audit_run` precise since-last-run delta + carry-forward SARIF merge — review loop log

Record for [`../specs/ANTS-1870-audit-run-precise-since-last.md`](../specs/ANTS-1870-audit-run-precise-since-last.md) § 8. Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-06-17)** — 2 reviewers (accuracy / consistency), briefed
  cold against live source. Verified + fixed:
  - **CRITICAL** — the struct⇄JSON bridge was unspecified and `recordRun`
    (`auditcache.h:93`) had no findings channel. Resolved by dropping the
    parallel struct: `computeDelta` now takes/returns `QJsonArray`
    end-to-end (§ 2.4), `recordRun` gains a `mergedFindings` param (§ 2.3),
    and `AuditCache::loadFindingsSidecar` is named as the reader.
  - **HIGH** — `top_findings_count` (default 0) ≠ `sampleCap`; the delta
    arrays now cap at `kSamplesPerToolDefault` (§ 2.8).
  - **HIGH** — new `Finding` struct collided with `AuditEngine::Finding`
    (`auditengine.h:82`); the `QJsonArray` design removes the struct (§ 2.4).
  - **HIGH** — `delta_unavailable_reason` precedence was undefined; § 2.7
    now orders the three reasons by data dependency.
  - **MEDIUM** — `auditcache` lane was stated as already present; § 7 now
    says to add it. Plus `computeDelta` edge-case enumeration (§ 2.4),
    INV-3/INV-8/INV-10 test-gap fills.
  - **LOW** — citation drift fixed: `computeFingerprint` call `:531`→`:529`;
    suppression `:589`/`:631`→`:586`/`:628`; parse-branch ranges;
    `Manifest.lastRun` `auditcache.h:57`→`:59`; forward-compat rule
    `:57-64`→`:54-56`; `prior_run` `:4604`→`:4605`; added a top-of-doc
    Contents index.
- **Loop 2 (2026-06-17)** — same 2 reviewers, cold (no briefing on Loop 1).
  Loop 1 CRITICAL confirmed resolved. Verified + fixed:
  - **HIGH** — `carriedForward` / `merged` were computed but the envelope's
    omission of them was unstated; § 2.8 now says they feed only SARIF +
    sidecar (counts only in the envelope) and INV-10 asserts it.
  - **MEDIUM** — current-run truncation had no run-level home; § 2.7 + § 2.8
    add `RunResult.findingsTruncated` (OR of per-tool
    `ParsedOutput.findingsTruncated`) and the prior-run read from the
    sidecar `"truncated"` field.
  - **LOW** — `parseToolOutput` `:534`→`:532`; `writeSarif` `:647`→`:646`;
    per-tool counts `:1657-1667`→`:1656-1668`; § 6 "canned `Finding` lists"
    → "canned `QJsonArray` finding lists"; loader path-resolution clarified.
  - **INFO surfaced (not fixed here, out of doc scope):** `auditcache.h:80`
    doc-comment "Returns true" is stale vs the `RecordedRun` return type;
    ANTS-1555 cites `cacheDirFor`/`cachePathFor` but the code exports
    `cacheDir`/`sarifPathFor`. Both logged for separate follow-up.
- **Loop 3 (2026-06-17)** — same 2 reviewers, cold. Loops 1–2 fixes held
  (no re-surfacing). Verified + fixed:
  - **HIGH** — § 2.1 claimed `parseToolOutput` "already" builds each
    finding object "unconditionally"; verified the construction is gated by
    `sampleCap` in *both* branches (JSON loop terminates on
    `samples.size() < sampleCap` `:561`; line branch `continue`s at `:630`
    before building `s`). Reworded to name the actual work: hoist `s` out
    of the `sampleCap` guard.
  - **HIGH** — whether a plain `auto`/full run seeds the sidecar was
    unstated (risking a perpetual `no_prior_findings`); § 2.3 now says every
    finding-bearing scope supplies its set, so the baseline is always
    seeded.
  - **HIGH** — truncation vs chain-integrity unreconciled; § 2.6 now states
    a truncated run records `truncated:true` and a later clean run
    self-heals the baseline.
  - **MEDIUM** — `merged` count-field asymmetry noted (§ 2.4); INV-2 made
    independently falsifiable in § 6.
  - **LOW** — dropped a wrong sibling-spec section number in this log.
- **Loop 4 (2026-06-17)** — same 2 reviewers, cold. Accuracy lane clean
  (all ~30 citations resolve). Verified + fixed:
  - **HIGH** — the new `recordRun` `mergedFindings` param, added with a
    default *before* the non-defaulted `priorRunOut` out-pointer, was
    illegal C++; § 2.3 now pins the target signature with `mergedFindings`
    trailing (`recordRun(canonProject, lastRunJson, priorRunOut,
    mergedFindings = {})`).
  - **MEDIUM** — § 2.3 now states the basename iso is `IsoNow.forFilename`
    and the JSON body's `iso_timestamp` is `IsoNow.forManifest`.
  - **MEDIUM** — INV-8 gains the readable-prior-`truncated:true` →
    `findings_truncated` case (the only prior-side path to that reason).
  - **LOW** — stale "pre-cold-eyes" Status line corrected; § 7 now also
    flags ANTS-1504's stale `:1481`/`:4211`/`:4260` cites for the same pass.
- **Loop 5 (2026-06-17)** — same 2 reviewers, cold. Accuracy lane clean
  (all ~30 citations resolve). Loops 1–4 fixes held. Verified + fixed:
  - **HIGH** — INV-2's *Test:* exercised `computeDelta`'s partition, not
    the fingerprint equality INV-2 claims; now asserts
    `computeFingerprint` equality directly.
  - **MEDIUM** — `merged` = `current` ∪ `priorOnUntouched` can approach
    2×`kSarifFindingsMax`; § 2.5 now pins the over-cap policy (keep
    `current` first, set `truncated:true`, fall to `findings_truncated`).
  - **MEDIUM** — INV-4 now asserts the basename hyphen-iso vs body
    colon-iso form split.
  - **LOW** — folded the stale `auditcache.h:80` "Returns true" comment
    into § 7's cleanup list.
- **Convergence (Loop 5, 2026-06-17):** the structural/architectural set
  (CRITICAL bridge, parser-restructure, `recordRun` signature legality)
  was resolved in Loops 1–4 and held without re-surfacing; Loop 5 yielded
  only test-precision + one localized cap-policy detail (all fixed). At the
  5-loop cap with no architectural finding outstanding, the loop converges.
- **impl-6540 (2026-11-17)** — implementation fold-back; no reviewer dispatched. ANTS-6540: each recorded finding carries `tool`, and `computeDelta` takes `toolsRanOk`; a prior finding counts toward `removed` only when its file changed and its tool finished ok (§ 2.4 step 1). Tests seen red first (`AuditRunDelta.Ants6540FindingOfAToolThatDidNotRunIsKept`).
