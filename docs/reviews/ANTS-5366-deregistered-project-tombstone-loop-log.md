# ANTS-5366 — loop log

Spec: [`docs/specs/ANTS-5366-deregistered-project-tombstone.md`](../specs/ANTS-5366-deregistered-project-tombstone.md)

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-10-10 | 2 (neutral-lane; every lane held every question; neither lane saw a git snapshot) | 1 | 0 | 2 | 1 | Verified 4, fixed 4, dismissed 2. Q3 (both lanes): § 2.3 put the visibility switch on file-local `readProjectWhere()` while § 2.4's checks call the public readers; the parameter is now named on `readProject()`, `readProjectBySlug()`, `readProjectByRoot()` and `listProjects()`. Q1 (both lanes): `slugCandidates()` reads `listProjects()`, so "need no argument" was false; it now passes `IncludeDeregistered`, with a leg in INV-3. Q3 (lane 2): the import refuses in plain text through id-only lookups; § 2.6 now names the readers it uses and the text it adds. Q4 (from a lane-2 question): INV-9 claimed a version-3 climb compared by every character; the test climbs from version 1 under `normaliseDdl`. Dismissed: `markUnresolvedLinks`' own-slug lookup (builds the same either way); § 2.7's "stop reading the store" (false, old builds only), corrected by deletion. Five open questions resolved clean (one production `registerProject` caller; the restore applies the legend; two digest readers; the handler compiles into `ants_mcpcore_lib`). |
