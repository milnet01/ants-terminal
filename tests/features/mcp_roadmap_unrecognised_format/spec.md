# mcp_roadmap_unrecognised_format

Feature-conformance test for **ANTS-1429** — `unrecognised_format`
gate in `cmdRoadmapQuery` and `cmdRoadmapLog`.

Locks in the contract that when a roadmap file exceeds
`kRoadmapMinParseableSize` (1024 B) and `RoadmapDialog::parseBullets`
returns zero hits, the MCP verbs return a typed error envelope
(`code:"unrecognised_format"`, with `path` + `bytes` + `hint`)
rather than the legitimate-but-misleading silent-empty shape.

## Invariants

- **INV-1.** `cmdRoadmapQuery` body contains the `ANTS-1429` anchor
  AND the `"unrecognised_format"` literal AND a reference to
  `kRoadmapMinParseableSize`. File-position bounded between the
  `cmdRoadmapQuery` and `cmdRoadmapLog` signatures.
- **INV-2.** `kRoadmapMinParseableSize = 1024` declared in
  `remotecontrol.h` next to `kRoadmapCacheTtlMs`.
- **INV-3.** `cmdRoadmapLog` body contains the same anchor +
  `"unrecognised_format"` literal AND assigns both `env["path"]`
  and `env["bytes"]` alongside the literal — envelope shape parity
  with the read-side gate.

## Sibling test

Behavioural coverage is the existing
`tests/features/remote_control_roadmap_query/`. This fixture is
source-scrape only.
