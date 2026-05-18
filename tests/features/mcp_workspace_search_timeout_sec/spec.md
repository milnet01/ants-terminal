# ANTS-1565 — `workspace_search` configurable wall-clock budget

See `docs/specs/ANTS-1565.md` for the full design + rationale. This
spec.md mirrors the invariants the test enforces.

## Contract

`workspace_search` accepts an optional `timeout_sec` integer arg that
overrides the default rg wall-clock budget. The default is raised from
2 s (ANTS-1248) to 5 s (ANTS-1565). The effective value is echoed on
every response envelope; on hard-kill the error envelope also carries a
fallback hint.

## Invariants

- **INV-1** — `kWorkspaceSearchHardKillMs` constant has the value 5000.
- **INV-2** — `cmdWorkspaceSearch` parses `timeout_sec` and clamps it
  against `kWorkspaceSearchMinBudgetMs` (1000) /
  `kWorkspaceSearchMaxBudgetMs` (30000); out-of-range falls back to
  default.
- **INV-3** — the hard-kill error envelope (when matches.isEmpty())
  carries a `hint` field naming the three viable next steps.
- **INV-4** — the ok:true and hard-kill envelopes both carry the
  effective `timeout_sec` field (in seconds).
- **INV-5** — the `workspace_search` inputSchema in
  `claudeintegration.cpp` declares `timeout_sec` as an integer property
  with `default: 5`, `minimum: 1`, `maximum: 30`.
- **INV-6** — the `workspace_search` tool description mentions
  `timeout_sec` and the new default of 5 seconds.
