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

## ANTS-2164 / ANTS-2165 — cadence-hardening + hotfix

Two test layers cover the hardened pipeline:

- **Source-scrape** (`test_release_rc_pipeline.cpp`, the C++ cases
  `Ants2164*` / `Ants2165*`) — the §2.1 helpers exist, the INV-1/2/3/4/8/9
  guards are wired into `new-rc`/`promote`, the drift check is a hard gate
  (INV-5), `promote` tags the frozen `^{commit}` (INV-6), `cycle` self-skips
  and is dispatched (INV-7), and `hotfix` publishes a non-prerelease tag with
  no `wednesday_guard`, refuses an off-main SHA, and is wired into the arg
  parser (ANTS-2165 INV-3/5/7).

- **Behavioural** (`cut_rc_behaviour_test.sh`, registered as the `cut_rc_behaviour`
  ctest, label `features;fast`) — drives the real `cut-rc.sh` against throwaway
  git repos (per-repo bare origin + a no-op `gh` shim + a drift stub,
  `--skip-build`), asserting exit codes and the resulting CHANGELOG/metainfo/
  debian/tag state. Cases: empty-RC refusal + `--allow-empty-rc` override
  (INV-1); `[Unreleased]` roll (INV-4); placeholder-promote refusal (INV-2);
  three-carrier date-stamp + frozen-commit tag with main ahead (INV-3/INV-6);
  the 14-vs-15-day stale boundary + `--force-stale` (INV-8); public-base
  refusal (INV-9); `cycle` self-skip and both-ready (INV-7); and the full
  hotfix path — off-main refusal, `rc_base_mismatch`, `hotfix_branch_exists`,
  conflict-abort (INV-6), and a no-RC hotfix asserting the minimal tree
  (no leaked feature) + `[H] > [N]` CHANGELOG order (ANTS-2165 INV-2/INV-4).

A `bash` script is exercised by a shell harness (the project already
registers shell feature tests — `hook_pack`, `claude_git_context_hook`);
the C++ layer keeps the cheaper structural scrape. Every behavioural case
was verified to FAIL against the pre-fix `cut-rc.sh` (17 of 28 assertions
fail pre-fix; the rest are override/boundary paths that coincide).
