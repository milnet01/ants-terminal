# Feature: per-project `.ants/project.json` settings file

Test contract for ANTS-2160 (`docs/specs/ANTS-2160.md`). Locks the
behaviour of the pure `ProjectSettings::load` loader and the
settings-aware `CodebaseIndex` / `DocsIndex` walks, plus the
dispatch-side wiring via source-grep.

`ProjectSettings::load(rootCanonical)` is a pure Qt6::Core helper: the
test drives it directly against `QTemporaryDir` fixtures (a hand-written
`.ants/project.json`), and exercises the codebase_index walk through the
exported pure builders `CodebaseIndex::build()` / `refresh()`, reading
the in-memory `Index.files` (`candidates()` itself is an unexported
file-static).

## Invariants under test (mirrors ANTS-2160 §3)

- **INV-1** — absent file → `load` all-`nullopt`; a `src/`-only `build`
  is unchanged.
- **INV-2** — `source_roots:["engine"]` → `build().files` indexes
  `engine/foo.c` (non-`src` layout).
- **INV-3** — declared `roadmap`/`changelog`/`docs_dir`/`specs_dir`
  surface from `load`; `docs_dir` redirects the `DocsIndex` walk.
- **INV-4** — root-escape entry dropped; a mixed array keeps valid
  entries.
- **INV-5** — non-existent path → dropped (`nullopt`); a file-typed
  `source_roots` → `build` falls back to `src` (non-empty `Index.files`).
- **INV-6** — malformed / non-object / wrong-typed / `null` → all-`nullopt`.
- **INV-7** — partial settings: only-present keys override.
- **INV-8** — case-mismatched path treated as non-existent.
- **INV-9** — nested overlapping `source_roots` → file de-duped (once).
- **INV-10** — empty array / blank string → treated as absent.
- **INV-11** — `source_roots` replaces the `src` default (not union);
  `tests/` still walked.
- **INV-12** — settings honoured on the warm `refresh()` path, not just
  `build()` (no spurious re-outline; `Index.files` stays scoped).
- **Wiring** — every consumer (`codebaseindex` / `docsindex` /
  `remotecontrol` finders + spec routing / `projectlayoutengine`) calls
  `ProjectSettings::load`.

## Pre-fix check

Against pre-implementation code `src/projectsettings.{h,cpp}` does not
exist (link/compile error) and the consumers do not consult settings, so
every behavioural assertion fails. Verified before wiring the
implementation.

Label: `features;fast`.
