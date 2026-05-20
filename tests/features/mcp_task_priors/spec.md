# mcp_task_priors — feature-conformance contract

**Theme:** `task_priors` MCP tool wiring + ranking-logic markers
(ANTS-1306). Full spec: `docs/specs/ANTS-1306.md`.

This is a source-grep harness (the same pattern as
`mcp_current_state` / `mcp_invariant_check`): `task_priors` is a
RemoteControl method that resolves the project root via
`resolveRootCanonical(m_main, req)`, which dereferences a live
`MainWindow` — so the verb cannot be exercised GUI-free. The test
instead locks the wiring contract and the load-bearing logic markers
against the source files.

## Invariants

- **INV-1**: `cmdTaskPriors(const QJsonObject &req)` is declared in
  `src/remotecontrol.h` and defined in `src/remotecontrol.cpp` with an
  `ANTS-1306` anchor comment. Source: docs/specs/ANTS-1306.md INV-1.
- **INV-2**: term extraction uses the three documented regexes — id
  `ANTS-[0-9]+`, the known-extension path set, and
  `[A-Za-z_][A-Za-z0-9_]{3,}` for terms — and a process-static
  stopword set. Source: ANTS-1306 §3.2 / INV-2/INV-3.
- **INV-3**: empty/no-term `description` refuses `bad_args`; unresolved
  root refuses `no_project`. Source: ANTS-1306 §3.6 / INV-4/INV-5.
- **INV-4**: the roadmap-card bucket composes over `cmdRoadmapQuery`
  with `status:"all"`, `limit:500`, `include_body:true`; the commit
  bucket composes over `cmdGitState` `op:"log"`. Source: ANTS-1306
  INV-7/INV-8.
- **INV-5**: spec ranking applies the `+5` id-in-filename boost and
  caps results; result buckets sort by score descending. Source:
  ANTS-1306 INV-6/INV-11.
- **INV-6**: caps `max_specs`/`max_cards`/`max_commits`/`max_adrs`
  default 5/5/5/3 and clamp to [1,20]. Source: ANTS-1306 INV-12.
- **INV-7**: the envelope carries `terms`, `ids`, `paths`, `specs`,
  `specs_count`, `roadmap_cards`, `cards_count`, `commits`,
  `commits_count`, `adrs`, `adrs_count`. Source: ANTS-1306 §3.5.
- **INV-8**: `MainWindow` registers `task_priors` via
  `registerToolProvider` with `CallerCwdContract::Required`. Source:
  ANTS-1306 INV-13.
- **INV-9**: `callerCwdContractFor` classifies `task_priors` as
  `Required`. Source: ANTS-1306 INV-14.
- **INV-10**: `kindForName` classifies `task_priors` as `"context"`
  (not `"other"`). Source: ANTS-1306 INV-15.
- **INV-11**: the token-cost ledger carries `task_priors` with
  `{1200, 6000}`. Source: ANTS-1306 INV-16.
- **INV-12**: the tools/list schema registers `task_priors` with a
  `description` and a `selection_hint`, `description` + `caller_cwd`
  required. Source: ANTS-1306 INV-17.

## Out of scope

Behavioural exercise of the ranking (requires a live MainWindow for
root resolution — covered by the GUI-coupled-composer source-grep
convention this project already uses for `current_state`).
