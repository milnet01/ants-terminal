# Feature: layout auto-detect + `project_settings` verb

Test contract for ANTS-2161 (`docs/specs/ANTS-2161.md`). Locks the pure
`ProjectSettings::detect` detector and `ProjectSettings::applyWrite`
writer/validator, plus the verb-layer + registration wiring via
source-grep.

`detect` / `applyWrite` are pure Qt6::Core helpers; the test drives them
directly against `QTemporaryDir` fixtures (a canonicalised root per case).
The verb glue (`cmdProjectSettings`) and its dispatch registration are not
unit-testable without `RemoteControl`/`MainWindow`, so the verb-layer
invariants (INV-5/6/10/13) are asserted by source-grep — the same shape
the ANTS-2160 suite used for its consumer-wiring check.

## Invariants under test (mirrors ANTS-2161 §3)

- **INV-1** — standard `src/` layout, no settings file → `detect` returns
  no suggestion (`present:false`, `sourceRoots` nullopt, empty reason).
- **INV-2** — an existing `.ants/project.json` → `present:true`, no
  suggestion (the detector never second-guesses a configured project).
- **INV-3** — code under `engine/` (no `src/`) → suggests
  `source_roots:["engine"]`, `defaultSourceCount 0`, `totalSourceCount 2`.
- **INV-4** — `applyWrite` + a real write round-trips through `load`; the
  file is world-readable (0644).
- **INV-7** — `set` merge preserves untouched keys, including unrecognised
  ones (forward-compat).
- **INV-8** — a `null` value removes a key; a `null`-only call on an
  already-absent key is a valid no-op (object returned, not `bad_args`).
- **INV-9** — write-time validation: absent / wrong-type / root-escape →
  `bad_path`; wrong shape (non-array) → `bad_args`; never writes on fail.
- **INV-11** — noise dirs (`node_modules/` …) skipped: neither suggested
  nor counted in the total.
- **INV-12** (amended ANTS-3390) — source at the repo root, no subdir →
  whole-root `source_roots:["."]` suggestion (was: no suggestion), so
  codebase_index can reach the depth-0 files.
- **INV-14** (ANTS-3369) — a miss with NO root source (`rootLevel==0`) →
  suggests ALL first-party source subdirs, count desc / name asc
  (`app/a.c`+`engine/b.c` → `["app","engine"]`, total 2).
- **INV-17** (ANTS-3390) — a miss with source loose AT the repo root
  (`rootLevel>0`) → whole-root `source_roots:["."]` (subsumes the depth-0
  files + subdirs); `retroarch.c`+`libretro-common/x.c` → `["."]`, total 2.
- **INV-19** (ANTS-3588) — a `source_roots` suggestion ALSO proposes the
  conventional aux layout keys present on disk. Positive: `engine/` (≥2 `.c`,
  a miss) + `tests/`, `docs/`, `docs/specs/`, `ROADMAP.md`, `CHANGELOG.md`
  (`.md` not indexable, so `docs/` is NOT a source subdir) →
  `sourceRoots==["engine"]`, `testRoots==["tests"]`, `docsDir=="docs"`,
  `specsDir=="docs/specs"`, `roadmap=="ROADMAP.md"`, `changelog=="CHANGELOG.md"`.
  **Superseded in part by INV-21** — the aux keys are no longer a ride-along
  on a `source_roots` suggestion. Pre-fix: the `Suggestion` struct has no aux
  fields → the test does not compile, so the assertions cannot false-green.
- **INV-21** (ANTS-4093) — the aux keys are proposed on the NO-OVERRIDE path
  too: standard `src/`+`tests/` + `CHANGELOG.md` → `sourceRoots` nullopt (the
  default walk covers the code) but `changelog=="CHANGELOG.md"`. Previously
  every aux field was nullopt here, so a project whose `src/` layout is fine
  got a reply about `source_roots` and nothing else while five keys sat
  undeclared and unmentioned — and the cheapest reading of that is "nothing to
  configure", which leaves the project in the state the verb exists to end.
  The reason string on that path now scopes its verdict to `no source_roots
  override needed`, and the verb envelope always carries `undeclared[]`: every
  recognised key with no declaration, derived from the same `kKeys` set as
  `declared` so the two cannot disagree.
- **INV-22** (ANTS-4092) — a gitignored top-level directory is excluded from
  the detect walk and named in `excluded[]`. `git check-ignore --stdin` is
  asked ONCE, over the candidate names, BEFORE any counting — the reporting
  project's ignored `projects/` is ~2 GB, and a tree that will be discarded
  must not be walked to discard it. This makes detect agree with
  `workspace_search`, which defaults to `respect_gitignore:true`. A root with
  no git repo, no git binary, or a timing-out git behaves exactly as before
  (empty ignore set): the probe narrows a suggestion, it never blocks one.
- **INV-20** (ANTS-3705) — `op:"detect"` echoes the stored declaration as
  `declared` (the recognised keys, verbatim) plus `declared_missing[]` naming
  every declared path that no longer resolves under the root. Read from the
  file rather than via `ProjectSettings::load()`, which DROPS such an entry —
  making a stale declaration indistinguishable from an absent one to every
  consumer. Both fields are omitted when empty, so a project with no settings
  file sees the pre-ANTS-3705 envelope unchanged.

- **INV-21** (ANTS-4648) — the `present:true` short-circuit (INV-2) returns
  before the walk, so `defaultSourceCount` / `totalSourceCount` are still
  their initialiser. `Suggestion::countsComputed` records whether the walk
  ran, and `op:"detect"` OMITS both counts when it did not — absent reads as
  not-taken, the way `compact:true` already treats an empty. `counts_computed`
  is emitted either way, so absence is never ambiguous with a build that
  predates the field. A 0 there was read as a measurement: a ~3000-line
  project was reported as containing no source at all.

- **INV-23** (ANTS-4815) — the recognised keys with no declaration are
  partitioned by whether the caller can act on them.
  `ProjectSettings::splitUndeclared` puts a key in `undeclared` when a
  candidate path is on disk (so `op:"set"` will accept it) and in
  `unavailable` when none is (so `op:"set"` refuses `bad_path`, and the entry
  could only ever be cleared by creating a directory the project has chosen
  not to have). The partition is exact: every undeclared key lands in one
  half and no key in both. A key counts as having a candidate when the
  `Suggestion` proposes one — which also carries an injected `ResolvedAux`
  path differing from the convention — or when the conventional path exists;
  the second half is what keeps the INV-2 short-circuit honest, since detect
  returns there having proposed nothing at all. Before the split, a project
  that legitimately keeps no specs carried those keys forever, and could not
  distinguish "not configured yet" from "deliberately has none" — which
  trains a caller to ignore the array, costing the cases where it is a real
  to-do list.

- **INV-5/6/10/13 (wiring)** — `project_settings` registered
  `CallerCwdContract::Required` in `claudeintegration.cpp`'s
  `callerCwdContractFor()` + `registerToolProvider`; the handler writes
  `.ants/project.json`, refuses `settings_exists` (init no-clobber),
  returns `written:false` (init no-op), refuses `unrecognised_format`
  (malformed existing), and uses `bad_args` for set's no-key guard.

## Pre-fix check

Against pre-implementation code `ProjectSettings::detect`/`applyWrite` do
not exist (compile error) and the verb/registration grep strings are
absent, so every assertion fails. Verified before wiring the
implementation.

INV-23 was proved red differently, because a missing symbol fails to
compile rather than fails an assertion. `splitUndeclared` was stubbed to
the pre-fix behaviour — every undeclared key into `undeclared` — and both
INV-23 tests failed on their `unavailable` assertions while their
`undeclared` assertions still passed, so neither can be satisfied by
classifying every key one way.

Label: `features;fast`.
