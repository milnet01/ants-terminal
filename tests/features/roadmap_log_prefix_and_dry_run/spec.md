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
- **INV-3** — A malformed `id_prefix` (e.g. `"1234"`, letter-free) refuses
  with `code:"bad_args"` and writes nothing (counter unchanged). Per
  ANTS-3492 a digit-led prefix is fine *if* it contains a letter (`3D_E`);
  only a letter-free prefix is rejected.
- **INV-4** — `dry_run:true` on `op:append` returns
  `{ok:true, dry_run:true, would_be_id, bullet, line}` and leaves
  ROADMAP.md and `.roadmap-counter` byte-for-byte unchanged.
- **INV-5** — `dry_run:true` on `op:append_batch` returns the would-be
  `would_be_ids[]` with `applied_count:0` + `would_apply_count:N`, and
  leaves both files unchanged.
- **INV-7** — (ANTS-4508) a preview reports its id under `would_be_id` /
  `would_be_ids` and **never** under `id` / `ids`; a real write is the
  mirror image. Measured: a probe reported it would allocate CFG-0145,
  the id went into a commit message written before the real write, and
  two commits had to be amended. The envelope does carry `dry_run:true`,
  and a caller reading a single field does not see it — while anything
  written against a previewed id is wrong if any other write intervenes,
  in a way nothing detects, because the id then names a different item or
  none. Same `would_*` naming as ANTS-4463's `would_write` on this
  envelope. *Test:* `roadmap_log_prefix.Ants4508PreviewIdIsNotReportedAsAnAllocation`
- **INV-6** — `op:append_batch` on a fresh / id-less roadmap derives the
  leaf prefix for every bullet (`DOOM-0001`, `DOOM-0002`, …).
