# Feature: the envelope close-tag scrub is as tolerant as the open-tag scrub

## Problem

`ClaudeIntegration::wrapMcpData` wraps every MCP tool result in an
envelope and scrubs the payload so a hostile file line, commit message
or scrollback chunk cannot forge the frame and have the bytes after it
read as trusted prose by the consuming assistant.

It scrubs two forms, and they do not tolerate the same thing:

- close: `</\s*ants_mcp_data\s*>`
- open: `<\s*ants_mcp_data\b[^>]*>`

The open form accepts anything up to the `>`, so an attribute-bearing
open tag is caught. The close form requires the `>` immediately after
optional whitespace, so a close tag carrying anything else is not.
A payload containing a close tag with a trailing token passes through
untouched.

The asymmetry contradicts the reasoning both scrubs were written under.
The close scrub's own comment says "some assistant tokenisers normalise
tag casing and whitespace before pattern matching, so the strict-case
sentinel from above isn't enough", and the open scrub was added
(ANTS-1670 M2) for "a consuming assistant that matches the open tag
tolerantly". Both assume a lenient consumer rather than a strict XML
parser — and a strict parser is exactly what would reject a close tag
with attributes. Under the design's own threat model the close form is
the one that needs the tolerance more.

`needsEscaping` in `src/doccitations.cpp` carries a copy of both
regexes so a citation can disclose that a line will be rewritten. Its
comment says the rewrite "must not be bypassed — the citation discloses
instead", so a divergence between the two files means a line gets
rewritten with no disclosure, or disclosed and not rewritten.

## Contract

The close-tag scrub MUST tolerate the same trailing content as the
open-tag scrub: `</\s*ants_mcp_data\b[^>]*>`.

The `\b` is load-bearing and is why the pattern can be widened safely.
The scrub's replacement sentinel begins `ants_mcp_data_` — `data` is
followed by `_`, a word character, so no word boundary exists there and
the sentinel is never re-matched by either pattern. Widening to
`[^>]*` without the `\b` would make the scrub consume its own output.

`needsEscaping`'s pair MUST equal `wrapMcpData`'s pair, so disclosure
and rewrite cannot drift.

## Invariants

**INV-1 — a close tag with trailing content is neutralised.** After
`wrapMcpData`, a payload containing one leaves the result with exactly
one close tag: the envelope's own.

**INV-2 — a plain close tag is still neutralised.** The widening does
not lose the case that already worked.

**INV-3 — an innocent payload is untouched.** The result still has
exactly one close tag, and the payload text survives, so the scrub is
not over-matching.

**INV-4 — the scrub does not consume its own sentinel.** A payload that
already contains the sentinel comes out with it intact rather than
recursively rewritten.

**INV-5 — `needsEscaping` mirrors `wrapMcpData`.** Source-grep: both
regex literals in `src/doccitations.cpp` appear in
`src/claudeintegration.cpp`.

## Scope

### In scope
- Behaviour tests against `ClaudeIntegration::wrapMcpData`, a public
  static needing no instance.
- A source-grep for the mirror, since `needsEscaping` has no header
  declaration.

### Out of scope
- The comment-marker scrub in the same function. A separate defence
  with its own reasoning (ANTS-1996), unchanged here.
- `wrapMcpDataRaw`, which deliberately does not scrub — it frames with
  a nonce verified absent from the payload instead.
- Whether the envelope should be XML at all.

## Regression history

- **ANTS-1294:** introduced the wrap and the close-tag scrub.
- **Indie-review 2026-05-14 lane-5 ME-2:** added case and whitespace
  tolerance to the close form, on the grounds that the consumer is
  lenient.
- **ANTS-1670 M2:** added the open-tag scrub with `[^>]*` tolerance.
  The close form was not widened to match.
- **ANTS-4457 (cold sweep 2026-08-18, verified 2026-09-08):** reported
  as "close-tag scrub tolerates less than the open-tag scrub (`[^>]*`
  on the open form only)". Verified against source and fixed. Locked by
  this spec.
