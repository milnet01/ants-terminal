# Every environment that runs the suite declares the tools it shells out to

**Why this exists.** On 2026-08-14 CI was red on `main` for several commits
with `{"code":"rg_failed","error":"cited_by: rg failed to start (is ripgrep
installed?)"}`. `ripgrep` was in none of `.github/workflows/ci.yml`'s package
lists (ANTS-4391). On 2026-08-26 the same defect broke the openSUSE Build
Service package: the fix had reached the workflow and not the RPM spec, whose
`%check` runs the same suite (ANTS-4717). The full suite was green locally
throughout, and it always would be — `rg` is installed on the development
host, so every local runner exercises a machine where the dependency is
present.

**No local execution can catch this class**, which is what makes it worth a
static check rather than a better test run. The only artefacts that know what
a runner or build VM will have are the recipes themselves, so the check
compares the source against every recipe instead of against the machine.

**ANTS-4717 is why this reads "every environment" and not "the workflow".**
Guarding one carrier turns a class defect into a queue of surfaces, each found
by a build.

## The invariant

**INV-1** — every tool in the REQUIRED set appears, on a non-comment line, in
every carrier in the ENVIRONMENTS set.

**REQUIRED** holds tools the test suite invokes with **no skip path**: a
missing one is a test failure, not a skipped check. `ripgrep` is its only
member today. `cited_by`, `workspace_search` and `co_change_family` all start
`rg` through `rcRunRg()`, and their tests assert on real results rather than
tolerating a refusal.

**ENVIRONMENTS** holds the carriers that run `ctest`:

| Carrier | Where it runs the suite |
|---|---|
| `.github/workflows/ci.yml` | the `build-test` and `build-asan` jobs |
| `packaging/opensuse/ants-terminal.spec` | `%check` |
| `packaging/archlinux/PKGBUILD` | `check()` |
| `packaging/debian/control` | declares for `override_dh_auto_test` in `rules` |

**Deliberately NOT in ENVIRONMENTS:** `packaging/flatpak/`. It builds with
`-DANTS_TESTS=OFF` and `flatpak-builder` runs no `ctest`, so a test-only tool
is correctly absent there. Adding a carrier that runs the suite means adding
it here.

**Deliberately NOT in REQUIRED:** `cppcheck`, `semgrep`, `bandit`, `gitleaks`,
`shellcheck`. Those are audit tools that self-disable when absent — the audit
engine reports them as incomplete rather than failing — so their absence from
a recipe that runs no audit is correct, not a defect.

## Why the match strips comments

The RPM spec discusses `ripgrep` in prose as well as declaring it, so a
substring match over the raw bytes passes on the comment block alone with the
`BuildRequires` line deleted. The check therefore drops whole-line `#`
comments before searching — a marker all four carriers share.

## What this check does NOT cover, stated so it is not mistaken for coverage

- **It does not check WHICH job or stanza declares the tool.** It asserts
  presence on a non-comment line of the file. A third ctest-running job added
  to `ci.yml` without `ripgrep` in its own package list would break CI and
  pass this test. Verifying that needs YAML and RPM structure the test does
  not parse, and a wrong parse would be worse than this bound.
- **It does not strip trailing comments**, only whole-line ones. None of these
  carriers puts a package name in a trailing comment.
- **It assumes one package name across distros.** True of `ripgrep`; a tool
  whose name diverges needs a per-environment spelling the table does not yet
  carry.
- **It does not verify the package name installs the binary.** `ripgrep` the
  Debian package provides `rg` the executable; nothing here proves that.
- **It does not check Qt or any other build-time component** against the
  recipes. That is a different invariant, tracked as ANTS-3791.
- **It does not run anything.** It is a source scrape, and it is cheap enough
  to run on every build for that reason.

## Build

Compiled into the **`test_claude`** bundle. Label `features;fast`. Reads each
carrier through a `SRC_*_PATH` compile definition, the same way
`release_rc_pipeline` reads `release.yml`.
