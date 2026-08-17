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
- **INV-17** — (ANTS-2148) the C family (`.c`, `.cxx`, `.hxx`) is admitted by
  `admittedSuffix` and outlined via the C++ regex set, so a C-only project
  yields a non-empty map (a `.c` free function surfaces as a symbol). The
  summary carries a soft `empty` flag = `file_count == 0`, so an empty map is
  distinguishable from a small one (survives `session_orient`'s trim).
- **INV-18** — (ANTS-2149) `file_outline` accepts `file_path` as an alias for
  `path`, and `codebase_index` accepts `path` as an alias for `file_path`
  (canonical name stays the source of truth; alias fills in only when absent —
  source-scraped in remotecontrol.cpp).
- **INV-19** — (ANTS-3468) `lane_files:true` on the no-selector summary
  augments each lane with a sorted `source_files` array of its NON-test paths
  and emits a `lane_digest_truncated` flag (global cap `kMaxLaneDigestFiles`);
  the default (opt-out) summary stays counts-only and byte-identical. The
  digest is deterministic (sorted lanes + sorted paths) so it keeps a warm
  re-serve byte-identical (304-stable). The `session_orient` bundle passes
  `lane_files:true` so its embedded map is navigable, not counts-only.
  (ANTS-3503) When the lane digest is empty because the project has no
  parseable `## Module map` (no file carries a lane), `lane_files:true`
  instead emits a flat top-level `source_files` digest (sorted non-test
  paths, same `kMaxLaneDigestFiles` cap + `lane_digest_truncated` flag) so a
  lane-less repo still gets a first-call code map; the field is present only
  when the fallback fires (the has-lanes shape is unchanged).
- **INV-20** — (ANTS-3390) a `source_roots:["."]` walk keys files by their
  bare repo-relative path (`app.cpp`, `sub/lib.cpp`), NOT `./app.cpp`. The
  walk base `<root>/.` makes `QDirIterator` yield `./`-prefixed `rel`, so
  `walkSubtree` strips a single leading `./`; without it, `findFile`'s exact
  `fe.path == rel` match (the `file_path` lookup) returns `found:false` for a
  bare query — the RetroArch-class gap ANTS-3390 closes — and `roleFor`'s
  `tests/`-prefix detection misses a `./tests/…` path. Retroactively repairs
  shipped ANTS-3393's `["."]` keys.

- **INV-21** — (ANTS-4419) an `empty` summary also carries `empty_reason`,
  `empty_hint` and `empty_detail`, and the reason distinguishes three
  conditions: `project_not_registered` (indexable source exists but not under
  `src/`/`tests/`, and no `.ants/project.json` declares where it lives),
  `declared_roots_hold_no_source` (a settings file is present but its
  `source_roots` hold nothing indexable), and `no_indexable_source` (the tree
  genuinely has none). Completes INV-17, whose boolean told a caller the map
  was empty but not whether it was *inapplicable* — a Charls_Site session read
  `empty:true` as "there is no code here" and fell back to grep on a real
  tree. Reuses ANTS-2161's `ProjectSettings::detect()` rather than adding a
  second layout analysis, and runs only on the already-empty path.
  **The branch ORDER is part of the invariant**: `detect()` performs no walk
  when the settings file is present, so `totalSourceCount` is `0` there by
  construction, and a gate testing the count before `present` reports a
  registered project as `no_indexable_source`. A **non-empty** summary gains
  none of these fields, so every already-working project's response stays
  byte-identical; the gate is `query()`'s own `empty` flag, which is set only
  on the summary path. Every field is a deterministic function of the tree, so
  a warm re-serve stays 304-stable.

The test must fail against pre-implementation source (no `codebaseindex.*`,
no `codebase_index` wiring) and pass after the feature lands.
