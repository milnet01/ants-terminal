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
