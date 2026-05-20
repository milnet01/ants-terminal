# Frozen-RC release pipeline — release.yml + cut-rc.sh (ANTS-1318)

Source-scrape conformance for the two project-local surfaces of the
ANTS-1318 frozen-RC pipeline: the GitHub Actions release workflow and
the `packaging/cut-rc.sh` orchestration script. Behavioural end-to-end
verification (actual tag cuts, `gh api` prerelease flags) is manual per
ANTS-1318 §10; this test pins the invariants that are checkable from
source so they can't silently regress.

## Invariants

- **INV-1 (INV-8 channel split)** — `release.yml` detects RC tags with
  a `-rc[0-9]+$` regex and routes the AppImage zsync
  `UPDATE_INFORMATION` through a computed `update_channel`, NOT a
  hardcoded `latest`. The stable branch sets the channel to `latest`;
  the RC branch sets it to the RC ref so stable users (latest channel)
  can't auto-update onto an RC.

- **INV-2 (INV-5 backstop)** — `release.yml`'s auto-create-on-404 path
  passes `--prerelease` for RC tags (gated on the `is_rc` output), so
  an RC release object is never marked as a normal (latest-eligible)
  release even on the manual `workflow_dispatch` path.

- **INV-3 (INV-5 in cut-rc.sh)** — `cut-rc.sh` creates GitHub releases
  with `--prerelease` for `new-rc` and `respin` (RC cuts), and WITHOUT
  `--prerelease` for `promote` (the public release).

- **INV-4 (INV-3 base-only)** — `cut-rc.sh` reads the base `X.Y.Z` from
  `CMakeLists.txt` and never writes a version-bearing file; the `-rc`
  suffix lives only at the tag / release title / AppImage filename.

- **INV-5 (one RC in flight, §4.4)** — `cut-rc.sh new-rc` refuses to
  cut an RC for a new base while a different base's RC has no public
  tag yet.

- **INV-6 (irreversible-action gate)** — `cut-rc.sh` only pushes tags
  and creates releases under an explicit `--push`; without it the
  script rehearses (local tag + printed commands).
