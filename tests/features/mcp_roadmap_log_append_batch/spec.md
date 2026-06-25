# Feature: `roadmap_log op:append_batch`

Part of **ANTS-1879**. Full design + invariants live in
[`docs/specs/ANTS-1879.md`](../../../docs/specs/ANTS-1879.md).

## Scope

- `RemoteControl::cmdRoadmapLogAppendBatch` — handler for
  `op:"append_batch"`.
- `RemoteControl::cmdRoadmapLogAppendBatchForTest` — test seam
  (m_main-independent, mirrors `cmdRoadmapLogFlipBatchForTest`).
- `RemoteControl::formatRoadmapBullet` — shared bullet-formatting
  helper extracted from `cmdRoadmapLogAppend:3293-3344`; called from
  BOTH `cmdRoadmapLogAppend` and `cmdRoadmapLogAppendBatch`.
- `cmdRoadmapLog` dispatch ladder routing `op:"append_batch"`.

## Invariants tested

- **INV-1** dispatch — source-grep `op == "append_batch"`.
- **INV-2** required fields → `missing_field`.
- **INV-3** per-bullet validation failure → `skipped[]` with
  `bullet_index` + appropriate code; rest of batch applies;
  `bytes_written` covers ONLY accepted blocks.
- **INV-4** all-skipped → `ok:true, ids:[], applied_count:0,
  skipped:[…], skipped_count:N`; ROADMAP.md + counter untouched.
- **INV-5** contiguous counter allocation.
- **INV-6** `id_hint` only honoured on first bullet; later ignored.
- **INV-7** order preservation in spliced output.
- **INV-9** `unrecognised_format` short-circuits the whole batch.
- **INV-10** source-grep: `formatRoadmapBullet(` is called from
  both `cmdRoadmapLogAppend` AND `cmdRoadmapLogAppendBatch`.
- **ANTS-2179** counter reconcile — the `.roadmap-counter` is a hint,
  not the sole source of truth. When it lags the file's true max
  `[PREFIX-NNNN]` id, both the single and batch append paths skip past
  the live max (`newId = max(counter, fileMax) + 1`), self-heal the
  counter, and surface `counter_advanced_to`. An explicit `id_hint` at
  or below the file max is refused `id_taken` (single) / skipped
  `id_taken` (batch) rather than written as a duplicate. A counter
  already ahead of the file allocates normally with no reconcile flag.

## Method

`QTemporaryDir` holds a synthetic `ROADMAP.md` + `.roadmap-counter`.
Each test calls `cmdRoadmapLogAppendBatchForTest(req)` directly,
asserts envelope + resulting file content. Source-grep tests check
dispatch wiring + shared-helper call sites.
