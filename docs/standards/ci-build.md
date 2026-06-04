<!-- ants-ci-build-standards: 1 -->
# Ants Terminal CI build standard

Project-local convention for what the CI workflows must guarantee about
a release. Not part of the shareable `/start-app` standards set — it
depends on this repo's `ci.yml` (pull/push validation) and `release.yml`
(AppImage build) split.

One invariant, plus a release-tooling corollary — learned the hard way.

---

## B1 — CI must build on the release's Qt / runner baseline

The `ci.yml` validation that gates every push MUST include a build on
the **same Qt and runner baseline the release AppImage is built on**
(`release.yml`'s `runs-on` + Qt). "CI green" must mean "the release
artefact compiles." If CI builds on a newer toolchain than the release,
green CI is a false signal.

**Why this is load-bearing (ANTS-1977).** For three weeks (v0.7.92 →
v0.7.93) every public release shipped with **no AppImage**: the build
failed to compile on the release runner, yet `ci.yml` stayed green and
nobody noticed. Root cause: `ci.yml` ran on `ubuntu-24.04` (newer Qt),
`release.yml` on `ubuntu-22.04` (Qt 6.2.x — `qt6-base-dev` is installed
unpinned from apt, so the exact patch is whatever the runner's repo
resolves to, not a guaranteed value). The `find_sources` MCP-tool test's
`Sandbox` fixture returned a `QTemporaryDir`-bearing struct by value —
legal only with the Qt 6.10+ move constructor present on 24.04, a hard
`use of deleted function` error on 22.04. The same gap hides the whole "compiles on new Qt, not
on the baseline" class: transitive-include reliance, newer-Qt APIs,
stricter/looser overload sets.

**Mechanism.** `ci.yml` carries the `qt62-baseline` job that configures
+ builds (tests ON, so test TUs compile) on the release runner image and
Qt. It need not run the full `ctest` — the newer-Qt `build-test` job
already does — but it MUST compile everything the release build
compiles, so a baseline-only error fails the push, not the release tag.

- The baseline-guard job's runner + Qt-providing apt packages MUST match
  `release.yml`'s. When `release.yml`'s `runs-on` / Qt baseline changes
  (per the global "bump runtimes deliberately" rule), the guard job
  changes in the same commit — otherwise the guard silently drifts off
  the real target and the gap reopens.
- Keeping a *newer*-Qt job too (forward-compat) is encouraged; the
  invariant is only that the baseline is always covered.

## Corollary — the release tooling gates on CI, not just a local build

`cut-rc.sh`'s `build_and_test` runs on the developer machine's Qt
(a far newer Qt than the baseline), so a local
green is not evidence
the release runner will build. Tag-cutting SHOULD confirm the
baseline-guard job (B1) is green on the tagged commit before publishing,
rather than trusting the local build alone. (ANTS-1978.)
