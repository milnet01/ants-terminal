# Feature: `codebase_index` MCP tool (ANTS-1637)

Pre-computed project structural map served by the Ants MCP so a Claude
session stops re-deriving project shape with grep / `file_outline` /
CLAUDE.md reads. Full design + invariants: `docs/specs/ANTS-1637.md`.

This conformance test drives the pure `CodebaseIndex` helper
(`src/codebaseindex.{h,cpp}`) against fixtures in a `QTemporaryDir`, and
source-scrapes the MCP wiring sites. It maps to the spec invariants:

- **INV-1** — cold `build` walks `src/`+`tests/`, one `FileOutline` pass per
  file, produces `files[]` + `laneToFiles`; a no-source root → `file_count:0`.
- **INV-2** — `refresh` re-outlines only changed+added, drops removed (from
  `files[]` *and* `laneToFiles[]`), assigns an added file its lane;
  `refreshedOut` = changed+added.
- **INV-3** — a map-source mtime change re-derives `laneToFiles` with
  `refreshedOut==0`.
- **INV-4** — ≥2 selectors → `bad_args`; no selector → summary.
- **INV-5** — `symbol=` returns `{path,line,kind}`; a miss → `found:false`.
- **INV-6** — `lane=` returns files+symbols; unknown lane → `found:false`
  with available lanes echoed.
- **INV-7** — `file_path=` found / not-found (handler routes through
  `PathValidation`, source-scraped).
- **INV-8/9/10** — caller_cwd Required / ETag-304 / `fields` projection wiring
  (source-scrape) + warm-query etag stability (behavioural).
- **INV-11/15** — file-count and byte ceilings → `files_truncated`, the
  deterministic-order prefix survives.
- **INV-12** — `toJson`→write→read→`fromJson` round-trip; atomic `QSaveFile`.
- **INV-13** — a version-0 / foreign-root / garbage cache rebuilds.
- **INV-14** — `lane=` symbol cap → `symbols_truncated`, first symbol kept.
- **INV-16** — summary `roles{}`/`languages{}` sum to `file_count`.

The test must fail against pre-implementation source (no `codebaseindex.*`,
no `codebase_index` wiring) and pass after the feature lands.
