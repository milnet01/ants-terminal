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
