# ETag short-circuit never speaks for a refusal

**ID:** ANTS-4446
**Status:** shipped
**Surface:** `ClaudeIntegration::applyEtagPattern` (`src/claudeintegration.cpp`)

## Contract

`docs/standards/mcp-tools.md`, § ETag-304: *"a refusal envelope is never
short-circuited."*

The dispatcher-level short-circuit honoured neither half of that.

1. It built `{ok:true, unchanged:true, etag}` **before** parsing the
   response, so it never learned the body it spoke for was a refusal.
2. Its tail attached an `etag` to any parseable JSON object regardless of
   `ok` — and every etag-supported verb's description tells the caller to
   cache the etag and send it back. Defect 2 is what made defect 1
   reachable.

`maybeInsertIdempotentReadCache` already guards its own store this way
(INV-5(a)/(b)). This function had no equivalent.

## Invariants

- **INV-1** — **A refusal is returned verbatim, with no `etag` attached.**
  A caller can only replay an etag it was given, so this is the half that
  makes INV-2 unreachable rather than merely guarded. *Test:*
  `Inv1RefusalGetsNoEtag`.
- **INV-2** — **A refusal is never short-circuited, even when the caller's
  `etag_match` equals that refusal's own hash.** *Test:*
  `Inv2RefusalIsNeverShortCircuited` — sends `etagFor(refusal)` back and
  asserts `ok:false` with its `code` intact, `unchanged` absent.
- **INV-3** — **Success is unaffected in both directions**: the etag is
  still attached, and a matching `etag_match` still yields
  `{ok:true, unchanged:true, etag}`. The guard against satisfying INV-1/2
  by disabling the pattern. *Test:* `Inv3SuccessStillShortCircuits`.
- **INV-4** — **An envelope with no `ok` key is a success, not a refusal** —
  `mcp::projectFields`' test verbatim, so the two agree on what a refusal
  is. *Test:* `Inv4NoOkKeyIsNotARefusal`.
