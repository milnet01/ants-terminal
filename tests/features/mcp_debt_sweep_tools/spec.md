# MCP debt_sweep_* Tool Wiring

Locks the wiring layer for ANTS-1113 v1's four `debt_sweep_*` MCP
tools (see `docs/specs/ANTS-1113.md` § 2.2). Source-grep style:
slurp the cpp/h files and assert each tool name + cmd method
exists across all three wiring layers.

## Invariants under test

- **INV-12a.** All 4 tool names appear in `claudeintegration.cpp`
  `tools/list` block.
- **INV-12b.** All 4 tool names appear in `mainwindow.cpp` as
  `registerToolProvider("<name>", …)` calls.
- **INV-12c.** All 4 `cmdDebtSweep*` methods are declared in
  `remotecontrol.h`.
- **INV-12d.** All 4 `cmdDebtSweep*` methods are defined in
  `remotecontrol.cpp` as `RemoteControl::cmdDebtSweep*`.
- **INV-12e.** All 4 schemas in `claudeintegration.cpp` use
  `additionalProperties:false` (so unknown keys are rejected).
- **INV-13a (ANTS-3345).** The `debt_sweep_scan` schema declares
  `limit` + `offset` props, and `cmdDebtSweepScan` emits the
  pagination envelope (`has_more` + `next_offset` tokens present in
  `remotecontrol.cpp`).
- **INV-13b (ANTS-3345).** `debt_sweep_scan` is in the
  `isOffloadEligible` allowlist (`mcpprojection.cpp`), so a large page
  spills to a `read_spill` pointer instead of inlining.
- **INV-13c (ANTS-3346).** The `debt_sweep_defer` schema declares the
  `triaged` prop, and `cmdDebtSweepDefer` calls `evaluateTriageGate` and
  can refuse with `needs_triage` (both tokens present in
  `remotecontrol.cpp`).
