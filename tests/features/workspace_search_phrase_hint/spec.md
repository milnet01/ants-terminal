# workspace_search multi-word phrase hint (ANTS-2045)

`workspace_search` matches its `query` as a single literal/regex pattern,
not as AND-combined terms. A natural-language query ("About modal RetroDB
version") silently returns zero matches even when each word exists —
reproduced across four CC sessions.

## Invariant

- **INV-1** — When a whitespace-bearing query returns zero matches, the
  `ok:true` envelope carries an advisory `hint` string explaining the query
  was matched as one phrase (and how to AND terms). Pure response-shaping:
  no change to search semantics, and a single-token / non-empty-result
  query never gets the hint.

## Out of scope

The optional OR-fallback that ranks by significant-term overlap (a separate
follow-up if the advisory hint proves insufficient).

## ANTS-5139 — a word-bounded group is already anchored

`rcShortBareAltTerms` returns no terms when the whole pattern is a single group
bounded by `\b` on both sides, capturing or not, so `\b(TODO|FIXME|TBD|XXX)\b`
draws no `regex_advisory`. The group must open right after the leading `\b` and
close just before the trailing one; any other pattern is judged per alternative
as before, so `tan|cosine` and `(tan)|\b(TBD)\b` still name `tan`. *Test:*
`WorkspaceSearchPhraseHint.Ants5139BoundedGroupIsAnchored`.
