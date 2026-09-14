# Feature: workspace verb write safety and argument bounds

## Invariants

**INV-1 — mutation_probe writes atomically.** `cmdMutationProbe` writes the
mutant and the restore through `QSaveFile`, never a truncating `QFile` open.

**INV-2 — build_target_for validates `cmake_path`.** `cmdBuildTargetFor`
runs `PathValidation::validatePath` on `cmake_path` before opening it, as it
does for `path`.

**INV-3 — file_outline caps `paths`.** `cmdFileOutline` refuses a `paths`
array longer than `kMaxOutlinePaths` with `bad_args`.

## Rationale

The ANTS-5096 performance pass found all three. A truncating write that failed
part-way left a cut-short source file whose original was only in memory;
`cmake_path` could name a file outside the project; and one call could outline
any number of files on the single MCP worker.

## Test surface

`test_workspace_verb_guards.cpp` reads `src/remotecontrol_workspace.cpp`
(located from the test's own path) and checks the three function bodies.

## Regression history

- **ANTS-5096:** the three defects above. Locked by this spec.
