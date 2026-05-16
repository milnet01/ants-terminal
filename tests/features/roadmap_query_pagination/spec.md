# Feature spec: roadmap_query pagination + auto-truncate (ANTS-1436)

Live test of ANTS-1437 reproduced the Vestige spillover on this
repo's own ROADMAP.md (74,636 chars on one line, exceeded the 25k-
token cap, spilled to tmpfile). ANTS-1436 adds offset/limit args
and a server-side measure-then-cut auto-truncate so full-active
responses come back in chunks the caller can parse directly.

## Invariants

- **INV-1 / default unchanged for small roadmaps.** No pagination
  args + roadmap fits under soft cap → envelope omits
  offset/limit/total/truncated/next_offset. Source anchor:
  `shouldEmitPaginationFields` returns false when no caller arg AND
  no auto-truncate.
- **INV-2 / explicit args echo back.** caller's offset/limit echo
  in the envelope after clamping.
- **INV-3 / auto-truncate fires on large filtered.** When
  `measureCutPoint(filtered) < filtered.size()` and caller didn't
  pass limit, envelope carries `truncated:true` + `next_offset`.
- **INV-4 / next_offset advances by slice.length.**
  `next_offset == offset + slice.length` when truncated.
- **INV-5 / offset past end returns empty.** `offset >= total`
  returns empty bullets[], offset == total, no next_offset.
- **INV-6 / section_index + offset/limit rejected.** mode:
  section_index with offset or limit returns
  `code:"bad_mode_combo"`. Source anchor: `ANTS-1436-INV-6` in
  cmdRoadmapQuery.
- **INV-7 / filter order: status/rollup before pagination.** Total
  reflects post-filter count; pagination operates on that set.
- **INV-8 / bad_args on non-numeric or negative.** `offset:"foo"`,
  `offset:-1`, `limit:0`, `limit:"bar"` all return
  `{ok:false, code:"bad_args"}`.
- **INV-9 / explicit limit honoured up to clamp.** `limit:500`
  returns up to 500 even if auto-pick would pick fewer; clamped to
  [1, 500].
- **INV-11 / one helper call site per emission branch.** The
  PaginationEngine::pageBullets call appears exactly twice in
  cmdRoadmapQuery (section + full-file). Source-scrape regression
  asserts call-site count.

## Test scope

Two-layer:
- **Pure-function tests** on `PaginationEngine::pageBullets` —
  edge cases (empty array, single bullet, exact-at-cap, just-over-
  cap, offset past end, explicit limit + auto-pick interaction).
- **Source-scrape** on `cmdRoadmapQuery` for the call-site count,
  bad_args / bad_mode_combo anchors, dispatch-forwarding pattern,
  schema entry.
