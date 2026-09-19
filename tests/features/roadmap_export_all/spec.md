# roadmap_export_all — exporting every project, and the command around it

Feature contract for **ANTS-3794**.
Parent spec: [`docs/specs/ANTS-3794-roadmap-store-backup.md`](../../../docs/specs/ANTS-3794-roadmap-store-backup.md)

§ 6.1 of the parent assigns **INV-1 to INV-6** to this directory.

## What this locks

- **INV-1** — `main()` handles `--export-roadmaps` before it constructs
  `QApplication`. A source check: the flag's first mention in `src/main.cpp`
  precedes `QApplication app(`.
- **INV-2** — `exportAllProjects()` writes one `<export_slug>.jsonl` per
  project, byte-identical to `exportProject()` for that slug.
- **INV-3** — one project's failure does not stop the others, and
  `runExportCommand()` returns 1. The fixture makes the first slug's
  destination a directory, which only that project's write can trip over.
- **INV-4** — with no file at `storePath`, `runExportCommand()` returns 2 and
  creates no store file.
- **INV-5** — an empty project list sets `error` and neither writes nor
  deletes anything in `dir`.
- **INV-6** — orphan `*.jsonl` files are deleted only after a run with no
  failure; `notes.txt` and `sub/x.jsonl` are never touched.

Each test is shown to fail against the red-first stubs before the
implementation lands.
