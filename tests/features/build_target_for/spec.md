# build_target_for — "which target owns this file?"

Contract for `tests/features/build_target_for/test_build_target_for.cpp`.
Owning roadmap item: **ANTS-3745**. There is no `docs/specs/` document — the
verb is one lookup over a file the project already keeps, which
`spec-format.md` § 1's skip test covers.

`BuildTargets::parse` / `ownersOf` / `gtestSuites` are pure — text in, answers
out — so every row drives them directly, with no MainWindow and no filesystem.
Two rows deliberately run against this project's **real** `CMakeLists.txt`,
because the defect ANTS-3745 was filed for is that the mapping is not
guessable, and a fixture that only proves the parser handles a fixture proves
nothing about that.

## What each row locks

| Row | Claim |
|---|---|
| `ParsesTheThreeDeclaringCommands` | `add_library`, `add_executable` and an `ants_add_*_bundle` wrapper all yield a target, with the right `kind`, and a wrapper's own `add_executable(${name} …)` body is not mistaken for a fourth. |
| `SourcesAreCollectedByShapeNotByKeyword` | A `LIBS` entry, a bare word and a target name never enter `sources`; a path does, from any position; a comment is stripped; a one-line block terminates at its own `)`. |
| `UnresolvableSourcesAreLeftUnowned` | A path behind a CMake variable is not attributed to a target — the honest miss the verb reports as `found:false` rather than naming the wrong one. |
| `GeneratorExpressionWrappedPathsResolve` | `$<$<BOOL:${X}>:src/a.cpp>` yields `src/a.cpp`, because this project's own lists use that form. |
| `GtestSuitesAreDeduplicatedInOrder` | `TEST`, `TEST_F` and `TEST_P` all count; a suite named twice appears once; first-seen order is preserved; a suite named mid-line or in prose does not count. |
| `LiveCmakeMapsTheTwoNonObviousBundles` | Against the real `CMakeLists.txt`: `tests/features/cold_eyes_engine/` is owned by `test_audit` and `tests/features/spec_conformance/` by `test_claude` — the two examples ANTS-3745 cites as proof recall does not substitute. |
| `LiveCmakeOwnsALibrarySource` | Against the real `CMakeLists.txt`: `src/buildtargets.cpp` is owned by `ants_core_lib`, so the verb answers for library sources and not only for tests. |

## What is NOT locked

The verb body (`RemoteControl::cmdBuildTargetFor`) needs a live MainWindow, so
its envelope is not driven here. What that body adds over the engine is string
composition — `build_command`, `ctest_filter`, `ctest_command` — and the
`found:false` hint. A defect there is visible on the first call; a defect in
the mapping is not, which is why the coverage sits at the engine.
