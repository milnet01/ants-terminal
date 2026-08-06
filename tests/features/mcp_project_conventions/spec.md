# mcp_project_conventions — feature-conformance contract

**Theme:** `project_conventions` MCP tool wiring + curated-table drift
guard (ANTS-1307). Full spec: `docs/specs/ANTS-1307.md`.

Source-grep harness for the wiring (same GUI-coupling constraint as
`task_priors` / `current_state`), plus a **behavioural drift guard**:
every `source` path baked into the curated table must exist on disk
(ANTS-1307 INV-5). The drift guard fails on broken code — rename
`coding.md` without updating the table and this test goes red.

## Invariants

- **INV-1**: `cmdProjectConventions(const QJsonObject &req)` declared
  in `src/remotecontrol.h`, defined in the remotecontrol TUs with an
  `ANTS-1307` anchor. Source: ANTS-1307 INV-1.
- **INV-2**: `task_type` validated against exactly
  `{feature, bugfix, refactor, docs, test}`; otherwise `bad_args`.
  Source: ANTS-1307 INV-2.
- **INV-3**: unresolved root refuses `no_project`. Source: ANTS-1307
  INV-3.
- **INV-4**: the two common rows (commits.md, coding.md) are present,
  and each task-type branch contributes its own rows. Source:
  ANTS-1307 §3.2 / INV-4.
- **INV-5 (drift guard)**: every `source` path emitted by the curated
  table exists as a file under the project root. Source: ANTS-1307
  INV-5.
- **INV-6**: `sources[]` entries carry `exists` resolved via
  `QFileInfo`. Source: ANTS-1307 INV-6.
- **INV-7**: the envelope carries `task_type`, `conventions`,
  `conventions_count`, `sources`, `sources_count`. Source: ANTS-1307
  §3.4 / INV-8.
- **INV-8**: `MainWindow` registers `project_conventions` via
  `registerToolProvider` with `CallerCwdContract::Required`. Source:
  ANTS-1307 INV-9.
- **INV-9**: `callerCwdContractFor` classifies `project_conventions`
  as `Required`. Source: ANTS-1307 INV-10.
- **INV-10**: `kindForName` classifies `project_conventions` as
  `"convention"` (not `"other"`). Source: ANTS-1307 INV-11.
- **INV-11**: the token-cost ledger carries `project_conventions` with
  `{400, 1500}`. Source: ANTS-1307 INV-12.
- **INV-12**: the tools/list schema registers `project_conventions`
  with a `task_type` enum prop, a `selection_hint`, and
  `task_type` + `caller_cwd` required. Source: ANTS-1307 INV-13.

## Out of scope

Behavioural exercise of the verb (requires a live MainWindow for root
resolution). The curated rule *text* is reviewed by humans against the
cited `source` doc; the test guards only that the sources exist.
