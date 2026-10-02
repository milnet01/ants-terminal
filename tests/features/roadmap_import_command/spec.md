# roadmap_import_command — restoring one project from its export

Feature contract for **ANTS-5244**, the restore that
[`docs/specs/ANTS-3794-roadmap-store-backup.md`](../../../docs/specs/ANTS-3794-roadmap-store-backup.md)
§ 5 deferred.

```
ants-terminal --import-roadmap <export.jsonl> <project-root>
```

`RoadmapExport::runImportCommand(storePath, file, root, out)` reads one
`<export_slug>.jsonl` written by `--export-roadmaps` into the store at
`storePath`, and ties the restored project to `root`. An export never carries
a root ([ANTS-3761](../../../docs/specs/ANTS-3761-roadmap-export-format.md)),
so without one no session could find the project.

## What this locks

- **INV-1** — `main()` handles `--import-roadmap` before it constructs
  `QApplication`, as `roadmap_export_all` INV-1 does for the export.
- **INV-2** — a restore is faithful and findable. A project exported from one
  store and imported into a store that does not exist yet exits **0**, creates
  the store, resolves by `projectIdForRoot(root)`, and re-exports byte for
  byte as the file it came from.
- **INV-3** — a restore never overwrites. When the store already holds the
  export's project, or holds `root` under another project, the command exits
  **1**, names `roadmap_migrate op:"deregister"`, and leaves the store
  unchanged. The root is bound in the same insert as the project row, so the
  store's `UNIQUE` columns refuse it and nothing half-restored remains.
- **INV-4** — bad arguments exit **2** and create no store: an export file
  that cannot be read, or a root that is not an existing directory.

## After a restore

The roadmap file is not rewritten. The command prints the next step: run
`roadmap_log op:"render"` from the project root.

## Red proofs

INV-2, INV-3 and INV-4 were run red against a stub `runImportCommand` that
returned `-1`. INV-1 was run red against `src/main.cpp` with the flag's
literal changed, then restored.
