# Per-test ctest TIMEOUT overrides (ANTS-3658).
#
# The gtest bundles are discovered with DISCOVERY_MODE PRE_TEST, so no test
# name exists at CMake configure time and a set_tests_properties() in
# CMakeLists.txt cannot see them. CTest reads TEST_INCLUDE_FILES in order at
# test time; CMakeLists.txt appends this file after the bundles are declared,
# and CTest applies the properties to the tests the discovery step then adds.
#
# Naming a test that does not exist is silently ignored here — `if(TEST ...)`
# cannot see lazily discovered tests either, so the drift guard lives in the
# test binary instead: mcp_audit_run.TimeoutOverrideNamesThisSuite fails if the
# name below stops matching a registered test.
#
# The bundle default is TIMEOUT 10 — a hang guard, not a runtime budget. Only a
# test whose honest runtime approaches it belongs here, and only with the
# measurement that earned the number.

# Dispatches real audit lanes over the source tree, so the runtime is dominated
# by page-cache state, not by compute: ~2 s warm, 9.5 s cold, and past 10 s
# under `ctest -j4` contention (measured 2026-07-27, ANTS-3658). At the default
# it went red on a busy machine and green alone — a signal a pre-push gate must
# not carry. 60 s still catches a genuine hang.
set_tests_properties(mcp_audit_run.Ants3605InProcessLanesDispatchedHeadless
                     PROPERTIES TIMEOUT 60)

# ANTS-3793 § 6 / INV-3 — the read seam's p95 benchmark. A timing assertion on
# a loaded host is a flake generator, so it must not run in the default suite
# alongside the ceiling assertions it shares an invariant with; § 6 spends a
# paragraph on why one ctest registration cannot be half-labelled.
#
# This is where that gets fixed. LABELS REPLACES the bundle's "features;fast",
# so the case leaves the default presets and the pre-push gate (both filter
# `-LE 'e2e|perf'`) and joins `ctest --preset=perf`, which the release
# checklist exercises. Same file, same reason, as the TIMEOUT above: PRE_TEST
# discovery leaves nothing for CMakeLists.txt to name.
#
# Building a 1,839-item store and reading it 23 times is minutes of SQL on a
# cold cache, which the 60 s bundle default would kill.
#
# RoadmapReadSeam.Ants3793LatencyCaseIsPerfLabelled is the drift guard: naming
# a test that does not exist is silently ignored here, so a rename of the case
# below would quietly put the benchmark back in the default suite.
set_tests_properties(RoadmapReadSeam.Inv3Latency
                     PROPERTIES LABELS "perf" TIMEOUT 300)
