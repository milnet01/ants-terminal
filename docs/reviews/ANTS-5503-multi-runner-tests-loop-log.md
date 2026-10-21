# ANTS-5503 multi-runner tests — review loop log

Record for [`../specs/ANTS-5503-multi-runner-tests.md`](../specs/ANTS-5503-multi-runner-tests.md) § Cold-eyes loop log.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|------|------|-------|----|----|----|----|---------|
| 1 | 2026-10-21 | 2 | 2 | 3 | 4 | 2 | 11 verified, 11 fixed, 0 dismissed; 3 from the packet build (pytest `-q` summary unrecognised; map steering pytest with ctest-only entries; INV-13 test expecting `collected` on ctest), 1 from a lane's open question (`op:"record"` reply fields), 7 lane-raised, 4 of them by both lanes. Fixed: ignore globs per runner and non-language files; pytest summary shape pinned and sniff order stated (one was collateral of the packet-build fix); go no-test run refuses, §1 narrowed; INV-3 `total` computed; out-of-root entry goes to `ignored_files`; `cargo test --no-fail-fast` (plain `cargo test` stops at the first failing binary, measured); INV-2 test adds a ctest log carrying a pytest line. Unrunnable region declared: go, jest and vitest are not installed, so their output-shape questions (vitest indentation, jest `todo` in total, go/vitest no-test output, `npx --no-install` behaviour) were not measured. Five open questions resolved clean. Partition: both lanes held every question. Loop 2 dispatched. |
