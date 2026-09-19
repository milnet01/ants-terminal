# roadmap_export_publish — publishing the export, and the backup records

Feature contract for **ANTS-3794**.
Parent spec: [`docs/specs/ANTS-3794-roadmap-store-backup.md`](../../../docs/specs/ANTS-3794-roadmap-store-backup.md)

§ 6.1 of the parent assigns **INV-7, INV-8, INV-9, INV-10 and INV-12** to this
directory. The test is a shell script over throwaway repositories: a bare
remote and two clones. A stub stands in for the binary through
`ANTS_ROADMAP_EXPORT_BIN`, so no network and no real store are involved.
Git's global and system config are pointed away, so this machine's hooks
never run on the throwaway repos.

It exits 77 (ctest `SKIP_RETURN_CODE`), naming the tool, when `git`, `flock`
or `sqlite3` is missing. That skip path is why those tools are not in
`tests/features/ci_workflow_deps`' required set.

## What this locks

- **INV-7** — the publish commit holds only `roadmap-export/*.jsonl`. A
  staged and an unstaged unrelated change survive untouched, and the
  `.jsonl.lock` file the stub leaves stays untracked.
- **INV-8** — when the upstream has a commit `HEAD` lacks, the script fails,
  never runs the export, and makes no commit and no push.
- **INV-9** — a failure leaves `error` set and `success` unchanged; a success,
  including "nothing to commit", empties `error` and advances `success`.
- **INV-10** — with the lock held, the script exits 3 and changes neither the
  record nor the repository.
- **INV-12** — `tools/roadmap-store-backup.sh` writes the `snapshot` record by
  the same rules, against a temporary store.
