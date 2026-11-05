# ci_report — feature contract (ANTS-6399)

The contract is [`docs/specs/ANTS-6399-ci-report-verb.md`](../../../docs/specs/ANTS-6399-ci-report-verb.md).
Its § 5 holds the invariants. Fixtures are `gh` JSON in its own shapes,
trimmed to the fields read. No test touches the network.

| INV | Test |
|---|---|
| INV-1 | `McpCiReport.Ants6399FlakyIsOneWorkflowOnOneSha` |
| INV-2 | `McpCiReport.Ants6399NearBudgetUsesTheJobSpan` |
| INV-3 | `McpCiReport.Ants6399UnreadBudgetIsNeverGuessed` |
| INV-4, INV-5 | `McpCiReport.Ants6399EveryFindingNamesRunsAndAdvisesNoRetry` |
| INV-6 | `McpCiReport.Ants6399SavedPathsStartNoProcess` |
| INV-7 | `McpCiReport.Ants6399OrientBlockOnlyForProblems` |
| INV-8 | `McpCiReport.Ants6399CappedFetchStillReports` |
| INV-9 | `McpCiReport.Ants6399FailedFetchSavesNothing` |
| INV-10 | `McpCiReport.Ants6399NoWorkflowsStartsNoProcess` |
| INV-11 | `McpCiReport.Ants6399ServedByMcpdWithoutMainWindow` |
| INV-12 | `McpCiReport.Ants6399ShellGuardIsAStepBudget` |

ANTS-6402 (claude-d5's cold review) added `McpCiReport.Ants6402*`: a flake
needs the same job to pass later; a matrix name matches one job; a
submodule URL is no GitHub remote; ctest is read as its own command; a save
step that runs after a failure; a `|` block holds no shell guard; jobs
pages; reader edges; a log's head and tail; a missing workflow in `unread`.

`McpCiReport.Ants6399LiveRunSavesTheReport` runs the whole live path against
a fake `gh` and reads the saved report back through `session_orient`'s block.
