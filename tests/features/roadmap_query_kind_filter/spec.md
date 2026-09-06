# ANTS-4836 — `roadmap_query` filters by `kind`

## Background

`kind` is write-only in practice. `roadmap_log` sets it, every read
surface returns it, and nothing selects on it — so "which review fixes are
still open?" means pulling every active bullet and sorting by hand.
Reported by LottoTracker.

`status` has had a filter since ANTS-1247 and refuses an unrecognised
value with `bad_status` plus the accepted set. This gives `kind` the same
shape, for the same reason the status refusal exists: a silently ignored
filter returns the FULL set, which reads as "nothing matches that kind" —
the worst possible answer, because it is indistinguishable from a real
empty result.

## Invariants

### INV-1 — `kind` narrows the list

A `kind` argument keeps only bullets whose kind equals it, case-folded.
It composes with `status`, `section` and `query`.

### INV-2 — an unrecognised kind REFUSES

`code:"bad_kind"`, with `accepted` naming the vocabulary — mirroring
`bad_status`. Not a silent full-set return.

### INV-3 — it survives the lean projection

`mode:"headline_only"` drops the `kind` field from each row (ANTS-4699),
and filtering happens before projection, so the filter still applies. A
caller asking a triage question wants the ids, not the field.

### INV-4 — absent means unfiltered

No `kind` argument returns what it returned before, and the envelope
echoes `kind` only when one was applied.
