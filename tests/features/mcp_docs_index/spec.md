# Feature test — docs_index MCP verb (ANTS-2139)

Behavioural invariants drive the pure `DocsIndex` helper; wiring invariants
source-scrape the registration sites. Full design: `docs/specs/ANTS-2139.md`.

| INV | Test | Asserts |
|---|---|---|
| 1  | `BuildShapeAndEmptyRoot`     | cold build over a fixture tree → known doc entry shape + heading; doc-less root → `doc_count:0`, `ok:true`. |
| 2  | `RefreshDeltaAddedRemoved`   | refresh re-scans changed+added, drops removed, reuses untouched; `refreshedOut == changed+added`. |
| 3  | `HeadingLevelsTitleLongLine` | ATX levels 1..3, 1-based line, first H1 → title; no-H1 → `""`; a line > `kMaxLineBytes` skipped. |
| 4  | `LinkExtraction`             | relative `.md` resolved against the doc dir, `#anchor` stripped, deduped+sorted; external/img/anchor dropped; a kept target need not be indexed. |
| 5  | `SelectorArity`              | ≥2 selectors → `bad_args`; empty-string selector → summary; no selector → summary keys. |
| 6  | `TopicScoreCapMiss`          | topic scored title×3/path×2/heading×1; `maxTopicHits` cap + `topic_truncated`; no-hit → `found:false`. |
| 7  | `DocPathFoundMissLinkedFrom` | indexed path → entry + `linked_from`; unindexed in-root → `found:false`. |
| 8  | `IdAllStemMatchesCaseSensitive` | `id=` returns every stem match (sorted), wrong case → `found:false`. |
| 9  | `WiringRegistered`           | `registerToolProvider("docs_index", Required)`; `callerCwdContractFor` row; handler routes `doc_path` through PathValidation. |
| 10 | `WarmServeStable` + scrape   | 4-call cold→warm→304→bust; `generated_at_ms` byte-identical across warm calls; handler emits no `etag`; in `isEtagSupportedTool`. |
| 11 | `FieldsProjectionNarrows` + scrape | `docs_index` in mcpprojection's compaction table; `mcp::projectFields` narrows a summary body (universal since ANTS-4524 — no `fields=` enrolment). |
| 12 | `DocCountCeiling`            | `maxIndexDocs:2` over 2 root + 1 docs/ → root docs kept, docs/ dropped, `docs_truncated`. |
| 13 | `SerialisationRoundTrip` + scrape | `toJson`→`fromJson` equality; `QSaveFile` atomic write. |
| 14 | `StaleCacheRebuilds`         | version-0 / foreign-root / garbage cache each rebuilds. |
| 15 | `ByteCeiling`               | tiny `maxCacheBytes` truncates to the deterministic prefix, `docs_truncated`. |
| 16 | `SummaryCountIntegrity`     | `docs.length == doc_count`; `Σ dirs.doc_count == doc_count`; `heading_count == headings.length`. |
| 17 | `StatusBestEffort`         | `**Status:**` line → value; absent → `""`, still indexed. |
| 18 | `LinkedFromCap`           | `maxLinkedFrom:1` over 2 linkers → length 1, `linked_from_truncated`, path-first survives. |
| 19 | `PerDocCaps`             | `maxHeadingsPerDoc` + `maxDocBytes` caps keep the prefix, entry still emitted. |
