# ants-mcpd usage snapshots (ANTS-5311)

The contract is `docs/specs/ANTS-5311-mcpd-usage-snapshots.md`; its § 3
invariants are the ones this directory tests, by the names given there.

- INV-1 `RoundTrip`
- INV-2 `LiveSnapshotNotClaimed`
- INV-3 `DeadSnapshotFoldsOnce`
- INV-4 `FoldPreservesDisplayedTotals`
- INV-5 `LoneLockAge`
- INV-6 `TokenUsageIncludesPeers`
- INV-7 `UntrustedFilesSkipped`
- INV-8 `ForwardedCallNotInSnapshot`
- INV-9 `SingleConfigSave`
- INV-10 `ConcurrentClaimOnce`

INV-8 and INV-9 hold of the code before ANTS-5311 and are regression
guards, proved red by mutation. Every case works in a temporary directory,
never the real data dir. Built into `test_claude` (INV-6 drives
`RemoteControl::cmdTokenUsage`). Label `features;fast`.
