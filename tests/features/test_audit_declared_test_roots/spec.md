# test_audit_declared_test_roots — a declaration outranks a failed sniff

ANTS-3708. `test_audit_partition` gated every call on framework detection:
a project with no `pyproject.toml` / `CMakeLists.txt` / `Makefile.test` /
`package.json` / `Cargo.toml` / `go.mod` got `{ok:false,
code:"no_tests_found"}` even when `.ants/project.json` declared
`test_roots`, the directory existed, and it held test sources. An explicit
`scope:"files:<csv>"` did not help either, because the gate ran before
scope resolution — so there was no way to say "these exact files are the
tests, just partition them".

Hand-rolled harnesses (a plain `main()` returning non-zero) ship no signal
file and are the least sniffable thing there is, which is exactly the case
`.ants/project.json` exists to answer. Reported by DOOM Ants
(`DOOM_Ants_Ants_MCP_Feedback.md`, 2026-07-28).

## INVs

- **INV-1** (declared roots rescue a failed sniff) — with no framework
  signal file anywhere and `.ants/project.json` declaring `test_roots`,
  `TestAuditEngine::partition` returns `ok` with `framework == "custom"`
  and chunks covering the declared root's source files. The dimension
  checklist is framework-agnostic, so `"custom"` costs the caller nothing
  downstream.
  *Test:* `Inv1DeclaredTestRootsRescueFailedSniff`.
  *Breaks when:* the framework gate returns before consulting
  `ProjectSettings::load`.

- **INV-2** (an explicit `files:` scope needs no framework) — with no
  signal file and no declaration, `scope:"files:<csv>"` partitions the
  named files as `framework == "custom"`. The caller naming the files IS
  the answer the sniff was looking for.
  *Test:* `Inv2ExplicitFilesScopeNeedsNoFramework`.
  *Breaks when:* the gate runs before scope resolution again.

- **INV-3** (a real framework still wins) — a project that ships
  `CMakeLists.txt` and matching `tests/*.cpp` keeps `framework == "ctest"`
  even when `test_roots` is also declared. The declaration rescues a failed
  sniff; it does not override a successful one.
  *Test:* `Inv3DetectedFrameworkNotOverridden`.
  *Breaks when:* the declaration is consulted before `detectFramework`.

- **INV-4** (declared roots also rescue a zero-file walk) — when a signal
  file IS present but its globs match nothing, the declared roots are
  retried before refusing. The framework label stays as detected, because
  the signal file really is there.
  *Test:* `Inv4DeclaredRootsRescueZeroFileWalk`.
  *Breaks when:* the second refusal returns without the retry.

- **INV-5** (the refusal names what was probed) — when neither a signal
  file nor a declaration exists, the `no_tests_found` error names the
  probed signal filenames and both escape hatches (`test_roots`,
  `scope:"files:"`), instead of only reporting that detection failed.
  *Test:* `Inv5RefusalNamesWhatWasProbed`.
  *Breaks when:* the message drops back to "no test framework detected".
