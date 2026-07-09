# mcp_focused_test — feature-conformance contract

**Theme:** `focused_test` MCP — `FocusedTest` resolution lib (behavioural)
+ `cmdFocusedTest` wiring (source-grep). Full spec:
`docs/specs/ANTS-1302.md`.

The pure `FocusedTest` lib is exercised GUI-free against
`QTemporaryDir` fixtures (these tests fail on broken resolution
logic). `cmdFocusedTest` itself shells out to `ctest` against a live
build + `MainWindow`, so its wiring is locked by source-grep (same
constraint as `verify_changes` / the other run-a-subprocess tools).

## Invariants (lib — behavioural)

- **INV-1**: `resolve([], map)` → `Selection::Full`. (ANTS-1302 INV-3.)
- **INV-2**: map mode — a mapped file yields `Selection::Map` with the
  de-duplicated pattern union. (INV-5.)
- **INV-3**: map mode — an unmapped, non-ignored, non-defaulted file
  forces `Selection::Full` (conservative core). (INV-4.)
- **INV-4**: map mode — an `ignore`-matched file is skipped and does
  not force full; mixed with a mapped file → `Selection::Map`. (§3.2/§3.3.)
- **INV-5**: map mode — every changed file ignored → `Selection::Full`.
  (INV-6.)
- **INV-6**: map mode — an unmapped file with non-empty `default`
  patterns → `Selection::Map` using the default. (§3.3.)
- **INV-7**: `loadCoverageMap` returns `valid=false` with
  `error ∈ {absent, bad_json, bad_schema}` for missing / malformed /
  wrong-`schema_version` files; `valid=true` for a good v1 map. (INV-2.)
- **INV-8**: `map.valid==false` selects heuristic mode; a source file
  yields a pattern, a non-source file does not; no patterns →
  `Selection::Full`. (INV-7.)
- **INV-9**: `buildCtestRegex(["a","b"])` == `"(a|b)"`;
  `buildCtestRegex([])` == `""`. (INV-8.)

## Invariants (MCP wiring — source-grep)

- **INV-10**: `cmdFocusedTest` declared in `remotecontrol.h`, defined in
  `remotecontrol.cpp` with an `ANTS-1302` anchor; `m_focusedTestInFlight`
  member exists. (INV-9/10.)
- **INV-11**: registered in `mainwindow.cpp` via `registerToolProvider`
  with `CallerCwdContract::Required`. (INV-18.)
- **INV-12**: `callerCwdContractFor` → `Required`; `rateLimitClassFor`
  → `Expensive`; `kindForName` → `"test"`; `kCosts` carries
  `{focused_test,{800,4000}}`; tools/list schema entry +
  `selection_hint`. (INV-19/20/21.)
- **INV-13**: the shipped `tests/coverage-map.json` is valid
  `schema_version:1` and `loadCoverageMap` accepts it. (drift guard.)
- **INV-14** (ANTS-3449): `cmdFocusedTest`'s ctest invocation runs in
  parallel (`-j`, capped at 4 and the host core count via
  `QThread::idealThreadCount`) so a broad change whose selection downgrades
  to the full suite finishes within the MCP transport read-timeout instead
  of surfacing as a spurious `-32000`; source-scraped in `remotecontrol.cpp`
  (`ANTS-3449` anchor + `idealThreadCount`).

## Out of scope

Running ctest end-to-end (needs a live build + MainWindow). The
0-match safety net and ctest parsing are covered by `cmdFocusedTest`
review + the reused `TestResCache::parseCtestOutput` tests.
