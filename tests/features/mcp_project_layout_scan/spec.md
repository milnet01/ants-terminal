# mcp_project_layout_scan — feature contract

Pure-function tests for the ANTS-1430 `project_layout` scan helper
on `ProjectLayoutEngine`. See `docs/specs/ANTS-1430.md` for the
full design + rationale; this is the test-side mirror.

## What this test guards

The `ProjectLayoutEngine` surface:

- **INV-1 / First call scans.** Calling `scanLayout(emptyDir)`
  returns an envelope with `roadmap.path` empty and the optional
  fields empty; `probedPaths` enumerates every well-known path
  the scanner statted.
- **INV-2 / Populated project.** Calling `scanLayout(dir)` on a
  project with ROADMAP.md, CHANGELOG.md, `docs/specs`,
  `docs/standards`, `docs/decisions`, `packaging/*.metainfo.xml`,
  and `.roadmap-counter` finds them all and reports them
  correctly. Format detection on ROADMAP.md returns `"ants-v1"`
  when the marker is present, `"github-task-list"` when GFM
  bullets are detected, `"unknown"` otherwise.
- **INV-3 / mtime invalidation.** `isStale(cached,
  scannedAtMs + 1000)` is false when nothing changed; advancing
  any probed path's mtime makes it true.
- **INV-4 / TTL invalidation.** `isStale(cached,
  scannedAtMs + (ttl_days + 1) * 86400000)` is true regardless
  of mtime state.
- **INV-5 / Envelope schema stable.** The JSON envelope carries
  exactly the documented field set with snake_case keys.
- **INV-6 / Round-trip fidelity.**
  `fromJson(toJson(env))` reproduces the envelope's data.

## Bundle

`test_audit` — engine-style pure-function test, same family as
`test_session_memory_engine` and `test_token_usage_engine`.
