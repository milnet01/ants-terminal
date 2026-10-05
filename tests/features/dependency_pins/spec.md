# A below-latest dependency pin needs a ledger row (ANTS-3428)

**Why this exists.** `docs/standards/dependencies.md` § 2 makes an
undocumented below-latest pin a defect, and § 3's Downgrade Ledger says when a
held pin is due for a retest. Nothing checked either, so both rested on
someone remembering. `tools/check-dependency-pins.py` checks them, and the
daily `release-audit.yml` workflow runs it.

## Invariants

**INV-1 — every mechanical pin is read.** A third-party
`uses: owner/repo[/path]@<40-hex sha>  # vX.Y.Z` in `.github/workflows/*.yml`,
and a `FetchContent_Declare` whose `GIT_REPOSITORY` is on github.com with a
`GIT_TAG`. A local `./` action and a `docker://` ref are skipped. A
third-party `uses:` that is not a 40-hex SHA with a version comment is a
finding: the version cannot be read, and a mutable tag is not trusted
(dependencies.md § 6).

**INV-2 — a pin behind latest with no ledger row is a finding.** Tags compare
by their numeric parts, a leading `v` ignored. Two tags with no numeric part
in common are a finding saying they could not be compared, never a pass.

**INV-3 — a ledger row stands in for INV-2, and its retest is checked.** A
§ 3 row whose *Dependency* cell names the pin (`owner/repo`, or the repo name)
suppresses INV-2's finding. If upstream's latest is newer than the row's
*Latest tested*, the row is reported as due for a retest. A row with an empty
*Latest tested* or *Re-test trigger* is a finding (§ 3: malformed).

**INV-4 — an action's SHA is the commit its comment names.** The pinned SHA
must equal the commit the comment's tag resolves to. A mismatch is a finding.

**INV-5 — what could not be checked is said, not passed.** Exit 0 means every
pin was checked and nothing was found; 1 means at least one finding; 2 means
no finding but at least one pin could not be checked (no network, no `gh`).
Each unchecked pin is named.

**INV-6 — the upstream answers can come from a file.** `--upstream <json>`,
shaped `{"owner/repo": {"latest": "<tag>", "tags": {"<tag>": "<sha>"}}}`,
replaces every network call. `--root <dir>` points the scan at a fixture tree.

**INV-7 — it runs every day.** `.github/workflows/release-audit.yml` has a job
that runs the script with `GH_TOKEN` set; a finding fails the job.

## Test

`test_dependency_pins.py` builds fixture trees and an upstream file, and
checks each invariant's finding and exit code. INV-7 is a text check on the
workflow. Red first: before the script exists, every case fails.
