# Feature: `dropped` roadmap items are published as 🚫

Contract: `docs/specs/ANTS-4977-dropped-status.md` § 3. This test carries its
invariants by the same numbers.

## Invariants

**INV-1 — a dropped item renders as 🚫.** A render of a store holding one
dropped and one `internal` item publishes `- 🚫 [ID] **headline**` for the
first and nothing for the second. A stored legend without a `dropped` key
renders the default row `🚫 Dropped (closed, not done)`.

**INV-2 — a 🚫 bullet round-trips.** Migration stores `dropped`, and a
re-render reproduces the bullet line byte for byte.

**INV-3 — pass-headings.** `dropped`, `abandoned`, `wontfix` and a bare 🚫
read back as 🚫. Through migration each is `dropped` with `asserted`
provenance. `passStatusKeyword("dropped")` is `dropped`.

**INV-4 — GFM.** A flip to `dropped` writes `- [x] 🚫 <text>`, and a query
reads it back as 🚫.

**INV-5 — `roadmap_log` accepts `dropped`** on append, append_batch, flip and
flip_batch, store-backed and markdown-backed. An unknown status refuses
`bad_status`, and the message lists `dropped`.

**INV-6 — no ship date.** A flip from shipped to dropped clears `shipped`.

**INV-7 — query.** `status:"dropped"` returns exactly the dropped items;
`active` and `shipped` return none of them; `all` returns them.
`section_index` counts them in `total_count`.

**INV-8 — feedback.** A finding whose id is 🚫 collapses under
`compact_resolved`.

**INV-9 — dialog.** With Dropped unticked no 🚫 card renders. `History` is
Done + Dropped. An absent `dropped` filter key reads as ticked. (Dialogs
bundle: `test_roadmap_dropped_status_dialog.cpp`.)

**INV-10 — detection.** A roadmap whose bullets are 🚫 detects as ants-v1 in
`detectRoadmapFormat` and in `ProjectLayoutEngine::scanLayout`.

**INV-11 — never open.** `isOpen("dropped")` is false, and a flip to dropped
of an item with no Layman line succeeds.
