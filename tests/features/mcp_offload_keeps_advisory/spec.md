# The unknown-arg advisory survives the result offload

**ID:** ANTS-4626
**Status:** shipped
**Surface:** the `tools/call` dispatch path in `src/claudeintegration.cpp`;
`mcp::withIgnoredArgs` / `mcp::ignoredArgs`; `mcp::offloadBody`.

## Problem

ANTS-2175 attaches `ignored_args:[…]` when a call passes a key the verb's
schema does not declare. ANTS-2094 then replaces an over-threshold body with
a fresh head+pointer envelope built from its own keys only. The advisory was
attached first and discarded second, so it reached the caller **only when the
response was small**.

That is backwards. The offload fires on the largest answers, and a filter
that was silently ignored is exactly what makes an answer too large — the
reply is a superset of what was asked for, which is the one shape a caller
cannot detect. A filter that matched nothing and a filter that never ran both
return rows, and the second returns more.

Measured both ways against the live socket before the fix, same bogus
`section_filter` argument to `roadmap_query`: the small reply carried
`ignored_args:["section_filter"]`; the spilled reply carried no such key
anywhere in the envelope.

## Invariants

- **INV-1** — **The advisory is re-applied after the offload**, so a spilled
  envelope carries `ignored_args` whenever the un-spilled one would have.
  *Test:* `Inv1AdvisorySurvivesOffload` runs the real
  `withIgnoredArgs`-then-`offloadBody`-then-`withIgnoredArgs` sequence and
  asserts the key is present on the final envelope.
- **INV-2** — **`offloadBody` alone still drops it**, which is what makes
  INV-1 load-bearing rather than incidental. Pinned so that a future change
  making `offloadBody` preserve keys does not silently turn INV-1 into a
  no-op assertion nobody notices. *Test:* `Inv2OffloadBodyAloneDropsIt`.
- **INV-3** — **A clean call gains no key.** An empty ignored list leaves the
  envelope untouched, spilled or not, so the advisory never becomes noise on
  a correct call. *Test:* `Inv3CleanCallGainsNoKey`.
- **INV-4** — **The dispatcher applies it on both sides of the offload.**
  Source-scrape: the dispatch path contains a `withIgnoredArgs` call after
  the `offloadBody` call site, not only before it. *Test:*
  `Inv4DispatchAppliesAfterOffload`.
