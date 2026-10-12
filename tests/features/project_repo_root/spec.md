# ANTS-5667 — an outer project folder resolves to its `repo_root`

The contract is [`docs/specs/ANTS-5667-outer-folder-repo-root.md`](../../../docs/specs/ANTS-5667-outer-folder-repo-root.md).
Its § 3 lists the invariants, and its § 6 names the test for each.

This folder holds the `ProjectRepoRoot` cases. INV-1, and INV-9's settings
case, are in `tests/features/project_settings/`.

Fixture: an outer folder with no `.git`, whose `.ants/project.json` holds
`"repo_root": "clone"`. Its `clone/` subfolder has a `.git` and a roadmap
migrated into a throwaway store under `XDG_DATA_HOME`.
