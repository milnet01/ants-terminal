# Release pipeline: `packaging/release.sh` and its helpers (ANTS-5577)

Every release is a full public release, cut when there is something
meaningful to ship. One command, `packaging/release.sh release`, checks the
tree, merges `[Unreleased]` into the version section, dates it, stamps the
packaging carriers, commits, builds and tests, runs the OBS staging gate,
tags, lets `release.yml` publish the release with its files attached, and
submits to OBS. The weekly RC cadence (`new-rc`, `respin`, `promote`,
`cycle`, `hotfix`) is retired.

Test surfaces:

- `release_behaviour_test.sh`, ctest `release_behaviour`: drives the real
  `release.sh` and `release-notes.sh` against throwaway git repos with a
  bare origin and stubbed `gh`, `obs-submit.sh`, `obs-status.sh` and
  drift check. Covers INV-1 to INV-16, INV-19 and INV-20.
- `obs_status_behaviour_test.sh`, ctest `obs_status_behaviour`: drives the
  real `packaging/obs/obs-status.sh` against an `osc` shim serving canned
  XML. Covers INV-18.
- `test_release_pipeline.cpp` (features bundle): source-scrape of
  `release.yml` and `release.sh`. Covers INV-17.

## Invariants

- **INV-1** `release` refuses off `main` and with a dirty tree. Nothing
  changes.
- **INV-2** `release` refuses when tag `vX.Y.Z` exists at a commit other
  than HEAD: that version is already public. The message says to bump
  first. Nothing changes.
- **INV-3** `release` refuses when there is nothing to release:
  `## [Unreleased]` has no entry (a `- ` bullet or a `### Added|Changed|
  Deprecated|Removed|Fixed|Security` heading) and no `## [X.Y.Z]` section
  with entries exists. Nothing changes.
- **INV-4** `release` refuses when the `X.Y.Z` section, after merging, has
  no `**Theme:**` line. Nothing is written. The Theme line may sit at the
  top of `[Unreleased]` (before the first `###`) or already in
  `## [X.Y.Z]`; it ends up in the `X.Y.Z` section.
- **INV-5** Merge. Every `[Unreleased]` entry ends up under `## [X.Y.Z]`;
  the `## [Unreleased]` heading remains with no entries. When `## [X.Y.Z]`
  already has entries, old and new are both present: exactly one
  `## [X.Y.Z]` heading, one heading per category, new bullets before old
  ones within a category, categories in the order Added, Changed,
  Deprecated, Removed, Fixed, Security. (The retired `roll_unreleased` was
  a no-op when the section already had entries.)
- **INV-6** Dating. The heading becomes `## [X.Y.Z] - YYYY-MM-DD` with
  today's date and an ASCII hyphen; an existing heading with an em dash or
  any other suffix is rewritten to that form. The metainfo
  `<release version="X.Y.Z" ...>` gets `date="<today>"`. The debian block
  for `X.Y.Z-1` gets today's date in its ` -- ` trailer.
  `packaging/obs/_service`'s `revision` param becomes `vX.Y.Z`. Other
  versions' entries are untouched, and no version number is edited.
- **INV-7** Those edits land as one commit on `main`, and `main` is pushed
  to origin before the staging gate runs: the gate's `<full-sha>` argument
  equals origin's `main` at that moment.
- **INV-8** A red staging gate (either script) exits non-zero, creates no
  `vX.Y.Z` tag locally or on origin, and names staging in its output. With
  `--skip-staging` the gate scripts are not called and a warning is
  printed. A missing or non-executable gate script refuses the release
  unless `--skip-staging`. The gate calls carry `OBS_PROJECT` set to the
  staging project, and run before any tag exists on origin.
- **INV-9** The tag is an annotated `vX.Y.Z` at the commit the gate tested,
  pushed to origin. No tag whose name contains `rc` is ever created. The
  real-project `obs-submit.sh` and `obs-status.sh` run only after the tag
  is on origin, with no arguments and not against the staging project.
- **INV-10** The script never runs `gh release create`; the workflow
  creates the release with its files attached. It looks for the workflow
  run (polling up to `RELEASE_POLL_MAX` times), waits on it with
  `gh run watch <id> --exit-status`, and verifies the published release
  with `gh release view`.
- **INV-11** Re-run after a red gate: after a fix commit with a new
  `[Unreleased]` entry lands on `main`, `release --push` succeeds, merges
  the new entry into `## [X.Y.Z]` beside the earlier ones (INV-5), and the
  tag is at the new HEAD.
- **INV-12** Resume: tag `vX.Y.Z` exists at HEAD and is not on origin (an
  interrupted run). Running again does not refuse, makes no new commit,
  pushes the tag and carries on.
- **INV-13** A refused `git commit` (a pre-commit hook exits 1) aborts with
  a diagnostic containing `ANTS-4865`, exits non-zero, creates no tag, and
  leaves the written files staged.
- **INV-14** After the tag is pushed, a failing real-project
  `obs-submit.sh` or `obs-status.sh`, a failed workflow run, a workflow run
  that never appears, or a release missing a file does not remove the
  tag. The run ends non-zero and names what needs attention.
- **INV-15** Rehearsal (no `--push`) mutates nothing: no commit, no tag, a
  clean `git status --porcelain`, origin untouched. It prints what it would
  do on lines containing `[rehearsal]`.
- **INV-16** `packaging/release-notes.sh X.Y.Z` prints the text between the
  `## [X.Y.Z]` heading and its first `### ` heading with the `**Theme:** `
  label removed and no category entries, then a line linking
  `https://github.com/milnet01/ants-terminal/blob/vX.Y.Z/CHANGELOG.md`. It
  accepts a heading dated with ` - ` or ` — `, and exits non-zero when
  there is no such section.
- **INV-17** `release.yml` has no RC handling (`is_rc`, `IS_RC`,
  `--prerelease`, `update_channel`); refuses a tag that is not exactly
  `vX.Y.Z` (anchored pattern `^v[0-9]+\.[0-9]+\.[0-9]+$`); sets
  `UPDATE_INFORMATION` with `gh-releases-zsync|milnet01|ants-terminal|latest|`;
  and creates the release with its files in one `gh release create`
  command (`"${UPLOADS[@]}"`) with notes from `packaging/release-notes.sh`.
  `release.sh` contains no `gh release create`, no `sed -i`, no
  `--prerelease`, no `wednesday`, and contains `[rehearsal]`.
- **INV-18** `packaging/obs/obs-status.sh [--rev N] [--require-tests]`
  waits until every building repository has finished source revision N
  (default: the package's current revision), judged by job history and
  never by a status word. All repositories with a row of `rev >= N` and
  `succeeded`: exit 0. A repository whose `_result` says `succeeded` but
  whose newest row is for an older revision is not finished: with polls
  exhausted, exit 2 (an older failed row is likewise not a failure). A row
  of `rev >= N` and `failed`: exit 1, naming the repository. A repository
  whose status is `excluded` or `disabled` is ignored. `--require-tests`: a
  succeeded repository whose log lacks the ctest summary line
  `100% tests passed, 0 tests failed out of <n>` exits 1. `_result`
  unreadable through every poll: exit 3.
- **INV-19** `release.sh` accepts only `status` and `release` with the flags
  `--push`, `--skip-build`, `--skip-staging`. Any other subcommand (there is
  no `new-rc`, `respin`, `promote`, `cycle` or `hotfix`) or flag exits 2 with
  usage on stderr.
- **INV-20** A non-zero `packaging/check-version-drift.sh` refuses the
  release. Nothing changes.

## Rationale

- 0.7.112 was nearly cut by `new-rc` with a week of entries still in
  `[Unreleased]`, because the old roll did nothing when the version section
  already had entries (INV-5, INV-11).
- The old `promote` published the GitHub release about 25 minutes before
  `release.yml` attached the AppImage, and its tag carried undated notes
  (INV-6, INV-10).
- OBS builds are tested in staging before publishing, every release
  (INV-7, INV-8, INV-9). Pushing the tag fires OBS's `trigger_services`
  between a script's `osc update` and its commit, so the real submit runs
  after the tag push and after its own update (INV-14).
- Two `osc results` status-word watchers misfired on 2026-09-30, one never
  ending and one ending while Fedora still built: job history per
  repository is the reliable signal (INV-18).

## Scope

In scope: the observable behaviour of the commands above, through their
command line, files, git state and the calls they make to `gh` and the OBS
scripts.

Out of scope: the real `gh`, `osc` and OBS; the build and test gate
(`--skip-build` in every fixture); how the script is written inside;
`release.yml` behaviour at run time (only its source is scraped).

## Regression history

The RC pipeline (ANTS-1318, ANTS-2164, ANTS-2165, ANTS-4865, ANTS-4869,
ANTS-4871, ANTS-4872) is replaced by this contract under ANTS-5577.
ANTS-4865's requirement (a refused commit aborts by name) is INV-13.
