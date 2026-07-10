# ANTS-1289 — VerifyEngine conformance

Engine-only feature test. Drives `VerifyEngine::loadGateConfig` and
`VerifyEngine::runVerify` against synthetic projects under
`QTemporaryDir`. No QProcess to ants-terminal itself — the gates run
real `/bin/sh -c` commands against synthetic config so the tests
exercise the path that production code walks.

Pairs with docs/specs/ANTS-1289.md.

## Invariants covered

- **INV-1 (config precedence)** — `.ants/verify.json` wins over
  auto-detect when both are present.
- **INV-2 (timeout)** — gate exceeding `timeoutSec / N` is killed
  and reported `passed:false, exitCode:-1, skippedReason:"timeout
  after Ns"`.
- **INV-3 (log capping)** — `logTail` honours `maxLogLines` AND the
  16 KiB byte cap; `logTotalLines` reports the unfiltered count.
- **INV-4 (path-traversal)** — `.ants/verify.json` symlinked outside
  the project root is rejected; engine falls through to auto-detect.
- **INV-5 (ctest parsing)** — captured ctest summary line sets
  `passedCount`/`totalCount`; failing-test names populate
  `failingTests`.
- **INV-6 (sequential + skip)** — tests gate auto-skips iff prior
  build ran AND failed; tests gate runs when build passes.
- **INV-7 (additionalProperties schema)** — covered by the MCP-layer
  test bundle (`mcp_verify_changes_tool`), not here.
- **INV-8 (RAM)** — implicit via INV-3 byte cap; no separate test.
- **INV-9 (PATH inheritance)** — documented, not asserted (v1
  carries the same env-inheritance model as the rest of the MCP
  surface).
- **INV-10 (orphaned-source lint, ANTS-3373)** —
  `findUnreferencedSources(root, addedSourcePaths)` returns those
  added source paths whose basename appears in NO CMakeLists.txt /
  *.cmake under `root`. A basename referenced in a nested `*.cmake`
  counts as referenced; one referenced only inside a pruned
  build/vendor dir (`isNoiseDir`) does NOT — so a generated build-tree
  CMakeLists can't mask a real orphan. Empty input short-circuits to
  empty (no tree walk). Input order/duplicates preserved. The MCP
  layer (`cmdVerifyChangesImpl`) parses the added `.cpp/.cc/.cxx/.c++/.c`
  files from the working-tree `git status --porcelain` snapshot and
  surfaces the result as an advisory `orphaned_sources[]` array — it
  does NOT flip `all_passed` (the build passed; the file just isn't in
  it), and is emitted only when non-empty so the default envelope stays
  byte-identical.

## Out of scope

- **Inverse check** (a CMake source-list entry pointing at a
  now-missing path) — the roadmap's "Optionally" half. Deferred: it
  needs real CMake source-list parsing (extracting quoted paths from
  `add_executable` / `target_sources`), a larger surface than the
  basename grep. Filed as a follow-up if it recurs.

- ctest-via-actual-ctest invocation (covered by the project's
  existing `ctest --preset=default` harness).
- pytest / cargo / npm parsing (v1 leaves those at `format:"plain"`).
- Concurrent-call safety: the engine is stateless; per-call only.
