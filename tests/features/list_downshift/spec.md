# list_downshift — auto-downshift a would-be-truncated list to its lean projection (ANTS-3543)

Canonical spec: [`docs/specs/ANTS-3543.md`](../../../docs/specs/ANTS-3543.md).

When `roadmap_query` / `workspace_search` would truncate a large result list on
the **auto path** (no explicit page size), the server first projects the whole
list to its existing lean shape (`headline_only`) and re-measures, so a scanning
caller keeps every row's identity instead of losing the tail. It truncates only
if even the lean list overflows.

## Covered invariants

Engine (`PaginationEngine::pageBullets`, pure):

- **INV-1** — default (absent) projector is byte-identical to pre-3543;
  `downshifted == false`.
- **INV-2** — downshift fires iff the projector is set AND `limit <= 0` (auto
  path) AND the fat set truncated; an explicit positive limit never downshifts.
- **INV-3** — a fired downshift projects a **copy of the full** `filtered` set
  and re-pages from `offset`: fits-when-lean → not truncated, all items present;
  overflows-when-lean → truncated, strictly more rows than the fat page.
- **INV-4** — `downshifted` defaults false; the input array is never mutated.

`workspace_search` (`RemoteControl::downshiftMatches`, pure):

- **INV-8** — honest `truncated` across the 4-case (scan-cutoff × lean-fits/
  drops) truth table; a fully-returned lean list reads as complete (no stale
  fat-cap `truncated:true`); `downshifted` + `headline_only` set on a fire.
- **INV-9** — a caller-requested `headline_only` (already lean) never downshifts.
- **INV-10** — `also_at` (dedup fan-out) preserved; fat `context_*`/`text`
  dropped; the fat tail is never resurrected.

Wiring (source-scrape over `remotecontrol.cpp` / `remotecontrol.h`):

- **INV-5/6/11** — gated projector at both bullet sites; truthy-only
  `downshifted` emit; the ANTS-1436 exactly-5-`pageBullets`-calls count survives
  the new 4th arg.
- **INV-7** — the verb forwards an explicit `limit` verbatim.
- Routing + **projector identity** — `cmdWorkspaceSearch` caps via
  `downshiftMatches` passing `rcApplyHeadlineOnly` (not an empty/wrong callback);
  the byte-cap lives in `downshiftMatches`.

## Fail-first

Against pre-fix source (no projector param, no `downshifted` field, inline cap in
the handler) the pure tests fail to compile / assert and the scrapes miss, per
the project's must-fail-first convention.
