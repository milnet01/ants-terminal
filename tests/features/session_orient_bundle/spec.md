# `session_orient` bundle verb — feature-conformance test

Locks the invariants in `docs/specs/ANTS-1883.md`. Composes three
ETag-eligible verbs into one envelope.

## Anchors

| INV | Test                              | What it checks |
|-----|-----------------------------------|----------------|
| 1   | `Inv1EnvelopeShape`               | `current_state`, `project_layout`, `sections_index` keys present in bundle. |
| 2   | `Inv2SectionsIndexModeAndStatus`  | Upstream `roadmap_query` call uses `mode:"section_index"` AND `status:"active"`. |
| 3   | `Inv3EtagAllowlistMembership`     | `session_orient` registered in `isEtagSupportedTool`. |
| 4   | `Inv4CallerCwdRequired`           | `session_orient` mapped to `Required` in `callerCwdContractFor`. |
| 5   | `Inv5ToolsListDescribesBundle`    | tools/list descriptor block exists for `session_orient`. |
| 6   | `Inv6PartialUpstreamFailure`      | bundle's `ok` reflects all-three success/failure. |
| 7   | `Inv7TokenCostBucketRegistered`   | `tokenCostFor` table carries an entry for `session_orient`. |
| 8   | `Inv8CodebaseIndexRefreshTrimmed` | bundle invokes `cmdCodebaseIndex` under a `codebase_index` key (eager refresh at session start) AND strips volatile `generated_at_ms`/`refreshed_files` to keep the ETag stable (ANTS-2140). |
| 9   | `Inv9FeedbackPendingScan`         | bundle surfaces the cross-session feedback backlog under a `feedback_pending` key, reusing `FeedbackFile::parse`, gated to the maintainer project by `docs/standards/mcp-feedback-files.md`, surfacing only files with an un-triaged `deltaPresent` (ANTS-1964). |
| 10  | `Inv10CodebaseIndexLaneDigest`    | bundle passes `lane_files:true` into `cmdCodebaseIndex` so the embedded map carries the per-lane `source_files` digest (navigable, not counts-only); deterministic digest keeps the ETag stable (ANTS-3468). |

## Pre-fix verification

Before the fix the literal `"session_orient"` is absent from
`claudeintegration.cpp` and `cmdSessionOrient` is undeclared. After
the fix, every test turns GREEN.
