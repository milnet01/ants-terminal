# roadmap_log — counter-prefix fallback + dry_run preview

ANTS-2076 / ANTS-2077. Behavioural test over the `*ForTest` entry points
(`cmdRoadmapLogAppendForTest` / `cmdRoadmapLogAppendBatchForTest`) against
a temp project whose leaf directory is named `DOOM_Fixture` so the
leaf-derived prefix is deterministic (`DOOM_Fixture` → `DOOM`).

Source feedback: `DOOM_Ants_MCP_Feedback.md` finding #1 (a fresh project
got `ANTS-0001` instead of a project-shaped prefix) + idea B (`dry_run`).

## Invariants

- **INV-1** — On a fresh / id-less roadmap (no counter IDs to sniff),
  `op:append` derives the prefix from the caller's leaf directory:
  counter `0` → id `DOOM-0001`. Never the hardcoded `ANTS`.
- **INV-2** — An explicit `id_prefix` overrides the prefix that would
  otherwise be sniffed from existing IDs: a roadmap full of `ANTS-NNNN`
  with `id_prefix:"ZOOM"` and counter `9100` yields `ZOOM-9101`.
- **INV-3** — A malformed `id_prefix` (e.g. `"1bad"`) refuses with
  `code:"bad_args"` and writes nothing (counter unchanged).
- **INV-4** — `dry_run:true` on `op:append` returns
  `{ok:true, dry_run:true, id, bullet, line}` and leaves ROADMAP.md and
  `.roadmap-counter` byte-for-byte unchanged.
- **INV-5** — `dry_run:true` on `op:append_batch` returns the would-be
  `ids[]` with `applied_count:0` + `would_apply_count:N`, and leaves both
  files unchanged.
- **INV-6** — `op:append_batch` on a fresh / id-less roadmap derives the
  leaf prefix for every bullet (`DOOM-0001`, `DOOM-0002`, …).
