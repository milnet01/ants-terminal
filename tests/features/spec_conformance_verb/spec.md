# spec_conformance — verb-layer conformance

Contract for `tests/features/spec_conformance_verb/test_spec_conformance_verb.cpp`.
Owning spec:
[`docs/specs/ANTS-4108-spec-conformance-verb.md`](../../../docs/specs/ANTS-4108-spec-conformance-verb.md).

The engine lane (`tests/features/spec_conformance/`) covers `SpecConformance::run`.
This lane covers only what the engine cannot reach: the MCP boundary. The handler
needs a live `MainWindow`, so it splits the way `spec_lint_verb` does — behavioural
rows drive the pure helper `RemoteControl::specConformanceBuildResponse`, wiring
rows source-scrape the registration sites.

## What each row locks

| Row | Claim |
|---|---|
| `Inv9EtagShortCircuitIsHandlerLocal` | A matching `etag_match` short-circuits to `{ok, unchanged, etag}`; a non-matching one does not; a refusal envelope is never short-circuited. And the negative that makes all three work: `spec_conformance` is **absent** from `isEtagSupportedTool`. |
| `PathIsRewrittenProjectRelative` | The envelope's `path` is the project-relative path, not the engine's absolute one. |
| `VerbContractMinimums` | `caller_cwd` Required at both declaration sites; `path` required and validated before the engine runs; `max_cases` forwarded **unclamped**; the verb is bucketed in `kindForName`. |

## Why the ETag 304 is handler-local, against `mcp-tools.md` step 7

The standard's step 7 has the dispatcher compute the etag
(`applyEtagPattern` → `etagFor(responseText)`) and forbids the handler from
emitting one. This verb cannot use that path: its envelope carries
`observations[]`, one measured-microsecond row per case (spec § 2.3), so a hash
over the whole response text differs on every run. Under the central path the
etag would never match, the 304 would never fire, and INV-9 — "a matching
`etag_match` short-circuits" — would be unsatisfiable by any correct
implementation.

So the engine computes the etag over the envelope **minus** `observations`
(INV-9), and the verb performs its own 304. The row asserting `spec_conformance`
is absent from `isEtagSupportedTool` is what keeps the two mechanisms from being
wired at once: adding it there would overwrite the engine's stable etag with a
timing-sensitive one and silently kill the cache.

`fields=` used to be declined for the same reason: central projection is skipped
only on a *central* 304, a handler-local one is invisible to it, and a caller
passing `etag_match` and `fields` together would have `unchanged` projected out
of its own 304 response. **ANTS-4524 made `fields=` universal, so there is
nothing left to decline** — the hazard is now floored inside `projectFields`,
which returns any envelope carrying `unchanged:true` whole. The guard covers
this verb without this verb doing anything, which is the point: declining an
argument is a thing a future verb forgets to do.

## Verified RED before the implementation landed

| # | Mutation | Result |
|---|---|---|
| M1 | `specConformanceBuildResponse` ignores `etagMatch` | RED — the short-circuit row finds `findings` in what should be a 304. |
| M2 | Short-circuit before checking `ok` | RED — the refusal row reports `unchanged` for a call that never ran. |
| M3 | Leave the engine's absolute `path` in place | RED — the rewrite row. |
| M4 | Add `spec_conformance` to `isEtagSupportedTool` | RED — the negative scrape row. |
| M5 | `qBound` the `max_cases` argument in the handler | RED — the unclamped row; an out-of-range value must refuse, not clamp. |
