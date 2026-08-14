# CI workflow declares the tools the suite shells out to

**Why this exists.** On 2026-08-14 CI was red on `main` for several commits
with `{"code":"rg_failed","error":"cited_by: rg failed to start (is ripgrep
installed?)"}`. `ripgrep` was in none of `.github/workflows/ci.yml`'s package
lists. The full suite was green locally the whole time, and it always would
be: `rg` is installed on the development host, so every local runner —
`ctest`, `tools/ci-parity.sh`, the pre-push hook — exercises a machine where
the dependency is present.

**No local execution can catch this class**, which is what makes it worth a
static check rather than a better test run. The only artefact that knows what
the CI runner will have is `ci.yml` itself, so the check compares the source
against the workflow instead of against the machine.

## The invariant

**INV-1** — every tool in the REQUIRED set below appears in
`.github/workflows/ci.yml`. The set holds tools the test suite invokes with
**no skip path**: a missing one is a test failure, not a skipped check.

`ripgrep` is the only member today. `cited_by`, `workspace_search` and
`co_change_family` all start `rg` through `rcRunRg()`, and their tests assert
on real results rather than tolerating a refusal.

**Deliberately NOT in the set:** `cppcheck`, `semgrep`, `bandit`, `gitleaks`,
`shellcheck`. Those are audit tools that self-disable when absent — the audit
engine reports them as incomplete rather than failing — so their absence from
a job that does not run an audit is correct, not a defect.

## What this check does NOT cover, stated so it is not mistaken for coverage

- **It does not check WHICH job declares the tool.** It asserts presence in
  the file. A third ctest-running job added without `ripgrep` in its own
  package list would break CI and pass this test. Verifying that needs YAML
  structure the test does not parse, and a wrong parse would be worse than
  this bound.
- **It does not verify the package name installs the binary.** `ripgrep` the
  Debian package provides `rg` the executable; nothing here proves that.
- **It does not run anything.** It is a source scrape, and it is cheap enough
  to run on every build for that reason.

## Build

Compiled into the **`test_claude`** bundle. Label `features;fast`. Reads the
workflow through the `SRC_CI_WORKFLOW_PATH` compile definition, the same way
`release_rc_pipeline` reads `release.yml`.
