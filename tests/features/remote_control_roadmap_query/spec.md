# Remote-control `roadmap-query` verb (ANTS-1117 v1)

User request 2026-04-30: surface the parsed ROADMAP.md as structured JSON
to a Claude Code session over the existing remote-control IPC, so Claude
can interrogate the roadmap without re-burning the 4500-line file as
input tokens. See `docs/specs/ANTS-1117.md`.

## Surface

- `RoadmapDialog::parseBullets(markdown)` — pure helper extracting one
  `BulletRecord` per top-level status-emoji bullet (`✅` / `📋` / `🚧` /
  `💭`) in document order.
- `RemoteControl::cmdRoadmapQuery()` — verb handler that loads the
  active tab's ROADMAP.md (cached on `mtime` per INV-10), runs
  `parseBullets`, and returns the unified `{ok, ...}` shape.

## Invariants

- **INV-1** `parseBullets` returns one entry per top-level
  status-emoji-prefixed bullet in document order.
- **INV-2** Each entry's `id` field matches `^ANTS-\d+$` when an
  `[ANTS-NNNN]` token appears in the bullet body; empty otherwise.
- **INV-3** All four legend emojis (`✅` `📋` `🚧` `💭`) are recognised;
  plain narration bullets without an emoji are omitted.
- **INV-4** Idempotent: calling `parseBullets` twice on the same input
  returns byte-identical output (no internal mutation).
- **INV-5** Multi-line bullet bodies (continuation indent) are folded
  into the same `BulletRecord` — the headline of a bullet whose body
  spans 3 lines is parsed from the first **bold** chunk regardless of
  newline placement.
- **INV-6** `Kind:` and `Lanes:` sub-lines are extracted into the
  record's `kind` (string) and `lanes` (string list) fields.
- **INV-7** `cmdRoadmapQuery` is registered in `dispatch()` and
  handles `cmd == "roadmap-query"`.
- **INV-8** When `m_main->roadmapPathForRemote()` is empty, the verb
  returns `{"ok": false, "error": "...", "code": "no_roadmap_loaded"}`
  per the unified error shape (no crash, no segfault).
- **INV-9** Cache wiring: `m_roadmapCachePath`, `m_roadmapCacheMtimeMs`,
  and `m_roadmapCacheBullets` are members on the class and the verb
  consults them before re-parsing.
