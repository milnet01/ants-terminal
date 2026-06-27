# Feature: read_regions batched multi-selector read (ANTS-2219)

## Problem

Assembling a mental model of a feature means many separate `read_region` /
`file_outline` / Read calls (a DOOM path-tracer session issued ~8). There was
no read-side mirror of `apply_edits`' batched writes (DOOM_Ants feedback S1).

## Contract

`read_regions(items:[…])` fetches several slices in one call. Each `items[]`
entry is `{path, + one selector: symbol | start_line[/end_line] | section,
etag_match?}`. caller_cwd Required.

- **RR-1 batch** — N items return `{ok:true, results:[…], count:N}`, one slice
  envelope per item in order; each `results[i]` is exactly what `read_region`
  would return for that selector (line range / symbol body / md section), with
  a project-relative `path`.
- **RR-2 per-item etag → 304** — every non-304 result carries an `etag`; an
  item whose `etag_match` equals its current slice etag collapses to a compact
  `{path, ok:true, unchanged:true, etag}` stub (re-read after editing one file
  re-sends only the changed slices).
- **RR-3 per-item failure isolation** — a bad/missing item path yields a
  per-item `{ok:false, code}` (e.g. `not_found`) while the batch stays
  `ok:true` and the other items resolve.
- **RR-4 arg validation** — a missing or empty `items` array refuses with
  `bad_args`; more than 64 items refuses with `too_many_items`.
- **RR-5 shared budget** — one `max_bytes` budget (default 512 KiB, 4 MiB
  ceiling) is consumed across the set in item order; on exhaustion the batch
  sets `truncated:true`.
- **RR-6 wiring** — `cmdReadRegions` (+ the `readOneRegion` helper) in
  remotecontrol.cpp; the `read_regions` tool schema in claudeintegration.cpp;
  the provider registered Required in mainwindow.cpp.

## Out of scope

- Parallel/async per-item reads — items resolve sequentially (the budget is
  shared and order-dependent).
- A top-level batch ETag — re-read economy is per-item via `etag_match`.
