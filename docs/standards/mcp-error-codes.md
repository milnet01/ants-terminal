<!-- ants-mcp-error-codes-standards: 1 -->
# MCP error-code taxonomy (ANTS-1353)

This document is the canonical home for the `code` field that Ants
MCP refusal envelopes carry. Every MCP tool that returns
`{ok: false, error: "...", code: "..."}` MUST pick a code from the
table below. Adding a new code is a deliberate decision — extend
this taxonomy in the same commit as the new refusal site.

## Why a taxonomy?

The 2026-04-14 indie review sweep flagged "comment promises A, code
does B" as the cross-cutting failure mode of the recent security
work — contracts were correct, implementations were one step short.
Refusal-code drift is a specific facet of that pattern:

- A caller dispatches on `code` to decide whether to retry, prompt
  the user, log, or fail loudly. Inconsistent codes force callers
  to fall back to string-matching `error` text, which rots.
- A new tool that picks a one-off code (e.g. `path_invalid`) when
  an existing one fits (`bad_path`) splits the taxonomy without
  anyone noticing — until a downstream consumer can't write
  one switch statement that covers both.

This standard is enforced by convention. Future ANTS-NNNN could
add a feature-conformance test that grep-scrapes new `code` values
against the table below.

## Categories

### 1 — Input validation (caller's argv is malformed)

| Code | Meaning | Examples |
|------|---------|----------|
| `bad_args` | A required argument is missing or has the wrong shape. Prefer the more specific `missing_field` when an argument is *wholly absent* (not malformed); reach for `bad_args` when the value is present but ill-formed (wrong type, bad enum, failed regex), or when one guard covers both absent-and-malformed at once. | `cmdRoadmapLog` `id_strategy:"foo"`; a `pass` that fails its regex; numeric arg parsed as non-numeric. |
| `missing_field` | A required field is *wholly absent* from the request — the absent-arg specialisation of `bad_args` (same § 1 family). The pervasive write-verb code for an empty required scalar/array: `roadmap_log` / `changelog_log` / `feedback_log` / `spec_log` field guards (~50 call sites). Distinct from `bad_args`, which the same verbs also emit when a *present* value is malformed. (ANTS-2128 documented this de-facto code rather than migrating ~50 sites; a verb is free to fold both into `bad_args` when one guard spans absent-and-malformed, as `roadmap_log`'s pass-headings path does — ANTS-2126.) | `roadmap_log op:"append"` with no `section` / `status` / `kind` / `source`; `op:"annotate"` with an empty `note`; `changelog_log op:"add"` with no `summary`. |
| `bad_id` | An `id`-typed arg is malformed, or a required id/path locator is wholly absent. | `spec_query`/`spec_log` with an `id` that isn't `<PREFIX>-NNNN` (any project prefix, e.g. `ANTS-1963` / `DOOM-0009`, ANTS-3356) / `phase_<NN>_<topic>`; `spec_log` with neither `id` nor `path` (ANTS-1309/1963). Note `spec_query` with neither `id` nor `path` is NOT a refusal — it lists the specs dir (ANTS-3360). |
| `bad_path` | A path argument fails NFC / control-char / canonicalisation / project-root anchor — via `PathValidation::validatePath` (ANTS-1295) for most verbs, or an equivalent hand-rolled canonicalise + root-anchor check where a verb doesn't route through the chokepoint (`audit_run` is one such: it never calls validatePath). | `audit_run suppressions:"path:../../etc/x.json"` (ANTS-3615); a symlink escape from inside the tree. |
| `bad_cwd` | `caller_cwd` is present but does not canonicalise to an existing **directory**. Distinct from `caller_cwd_required` (wholly absent) and from `bad_path` (a *path argument*, not the cwd). Emitted by the shared `resolveInflightCallerCwd` helper in the dispatch lambdas of `audit_run`, `audit_poll` and `indie_review_dispatch`, before the verb's engine runs — so for those this, not `bad_path`, is what a caller sees for a bad cwd. `cmdLaunch` / `cmdNewTab` also emit it, but for a *different* condition: their `cwd` argument containing a control character or backslash, rather than failing to resolve. Compare `no_project`, which several root-resolving *write* verbs emit for the same condition. | `audit_run caller_cwd:"/no/such/dir"` (ANTS-1833). |
| `bad_tool` | A named entry in a tool-list argument isn't one the verb knows. | `audit_run tools:["cppcheque"]` — not in `kKnownTools()`. |
| `bad_scope` | A `scope` argument is malformed, or its embedded value fails sanitisation. | `audit_run scope:"since-tag:--evil"` — the tag fails `^[A-Za-z0-9._/+-]{1,128}$` or leads with `-`. |
| `bad_severity_floor` | A `severity_floor` argument isn't one of the accepted levels (`error`, `warning`, `note`). | `last_audit_summary severity_floor:"catastrophic"`. |
| `bad_actionable` | `test_audit_fold_in`: the `actionable[]` payload is malformed or empty. Refused *before* any id is allocated, so a bad payload can't permanently burn counter ids. | `test_audit_fold_in actionable:[]`. |
| `narrative_md_required` | A verb in narrative mode was given no `narrative_md` body (empty or absent). | `test_audit_fold_in narrative_mode:true` with no `narrative_md`. |
| `unknown_dimension` | A named review/audit dimension isn't in the engine's canonical set. | `test_audit_partition dimensions:"csv:foo"` — not in `kDimensions`. |
| `bad_chunk_id` | A `chunk_id` isn't present in the partition it references. | `test_audit_brief chunk_id:"c-999"` against a 16-chunk partition. |
| `compile_commands_escape` | A `compile_commands.json` include argument (`-I`/`-isystem`/`-iquote`/`-include`) resolves outside the project root and isn't under a known system-include prefix. Refused before the first clazy/clang-tidy spawn, so a poisoned compile DB can't widen the tool's read surface. | `audit_run` on a repo whose build DB carries `-I../../../etc` (ANTS-1446). |
| `not_feedback_file` | A `path` arg is well-formed and in-bounds but its resolved basename is not a `*_Ants_MCP_Feedback.md` file (the `feedback_query` / `feedback_log` suffix guard, ANTS-1961/1962). Distinct from `bad_path` (which is a malformed/escaping path) — this path is fine, just the wrong kind of file. | `feedback_query path:"notes.md"`. |
| `not_v2` | `feedback_log op:"compact_resolved"` (ANTS-3443) against a feedback file whose version marker is not `<!-- ants-mcp-feedback: 2 -->` (a `: 1` marker, or malformed/absent). The op needs the structural `**Proposed ID:**` line a v1 file predates, so on v1 it would be a silent no-op; refuse and direct the caller to `op:migrate_v2` first. **Known defect (ANTS-3621):** the gate tests `!= 2` rather than `>= 2`, so it also refuses *newer* markers (a `: 4` file) and then advises `migrate_v2`, which is wrong for a forward version. | `feedback_log op:"compact_resolved"` on a `<!-- ants-mcp-feedback: 1 -->` file. |
| `not_v1` | The mirror of `not_v2`: `feedback_log op:"append_tracking"` (ANTS-3477) against a **v2** (`<!-- ants-mcp-feedback: 2 -->`) file. append_tracking is the retired v1 tracking-table write — on a v2 file it would re-grow a maintainer table the inline-ID model replaced; refuse and direct the caller to `op:assign_id` (fill each finding's `**Proposed ID:**` line in place). A v1 / un-migrated file (marker `< 2`, incl. absent) stays valid. | `feedback_log op:"append_tracking"` on a `<!-- ants-mcp-feedback: 2 -->` file. |
| `bad_mode` | A mode-enum argument doesn't match any allowed value. Also reused for the `op`-enum guard in `feedback_log` / `spec_log` (an `op` outside the allowed set). | `roadmap_query mode:"foo"`; `feedback_log op:"bogus"`. |
| `bad_mode_combo` | A mode + other-arg combo is conceptually exclusive. | `roadmap_query mode:"section_index"` + `section:"x"`. |
| `bad_section` | A `section` slug isn't in the roadmap's heading index. | `roadmap_query section:"nonexistent"`. |
| `bad_case` | A slug or id locator differs only in case from a real entry; envelope carries the canonical form. | `roadmap_log section:"Performance"` when the slug is `performance`. Returns `canonical_slug:"performance"`. Used by `roadmap_query`, `roadmap_log op:append`, `roadmap_log op:append_batch`, `roadmap_log op:create_section`, and `spec_query`. |
| `bad_id_format` | An `id`/`ids` or flip/annotate locator is id-token *shaped* (`<prefix>-<digits>`) but fails the canonical `[PROJ-NNNN]` gate — a letter-leading prefix per roadmap-format.md § 3.5.1 (e.g. a digit-leading `3D_E-0022`). Such a token is never adopted as a bullet id, so the honest reply is a named refusal, not a silent `found:false` / `bullet_not_found`. Envelope carries the offending token (`id` / `bad_format_ids[]` / `locator`) + a canonical-form `hint`. Distinct from `bad_id` (which covers a malformed or absent id arg generally); this one fires on a *well-shaped but non-canonical* token. | `roadmap_query id:"3D_E-0022"`; `roadmap_log op:"flip" id:"3D_E-0022"` (ANTS-3387). |
| `bad_status` | A status-typed arg — a query *filter* or a *write value* — doesn't match the enum. | `roadmap_query status:"foo"`; `feedback_log` tracking-row `status` outside the `{📋 🚧 ✅ 💭 🔄 ❓}` set (ANTS-1962). |
| `bad_kind` | A `kind` enum arg doesn't match the recognised set (per roadmap-format.md § 3.5.3). | `roadmap_log kind:"weird"`. |
| `bad_level` | The `level` arg under `roadmap_log op:create_section` is not 2 or 3. | `roadmap_log op:create_section level:5` (ANTS-1878). |
| `bad_intro` | The `intro_body` under `roadmap_log op:create_section` contains a line matching `^#{1,6}\s` (would silently add a heading to the index). | `intro_body:"## stray"` (ANTS-1878). |
| `bad_title` | The `title` arg slugifies to the empty string (all non-letter-or-number characters). | `roadmap_log op:create_section title:"!@#"`. |
| `slug_collision` | The computed slug under `op:create_section` already exists in the index. Envelope carries `computed_slug`. | `roadmap_log op:create_section title:"Performance"` when a `performance` slug exists (ANTS-1878). |
| `headline_empty` | A bullet's `headline` field is empty. | `roadmap_log op:append_batch` bullet with no `headline` field (joins `skipped[]`). |
| `no_roadmap` | `caller_cwd` doesn't canonicalise to a directory, or no ROADMAP.md was found under the resolved root. | `roadmap_log caller_cwd:"/no/such/path"`. Used by every `roadmap_log` op (ANTS-1424, 1878, 1879), and by `test_audit_recheck`. **Not** `test_audit_fold_in` — it surfaces a missing/unwritable roadmap as `write_failed`. Semantic twin of `no_changelog` in § 2 — the two are categorised differently for historical reasons only. |
| `bad_feature_name` | A `feature_name` arg doesn't match the allowed pattern. | `plan_template feature_name:"!!!"`. |
| `missing_name` | A name-typed required arg is empty. | `tool_info name:""`. |
| `rate_limited` | The caller exceeded the per-tool sliding-window cap (ANTS-1356). The envelope carries `retry_after_ms`. | `audit_run` 11th call within 60 s (Expensive tier cap = 10/min). Caller should honour `retry_after_ms` before retrying. |
| `reports_dir_outside_root` | `reports_dir` resolves outside the focused project root AND `allow_outside_project:true` was NOT passed (ANTS-1455). Replaces the pre-ANTS-1455 `reports_dir_missing`. | `test_audit_synthesis_prompt reports_dir:"/tmp/foo"` without `allow_outside_project:true`. Caller's natural fix is to pass `allow_outside_project:true` for ephemeral `/tmp` workflows. |

### 2 — Resource state (the requested object isn't where the tool can act on it)

| Code | Meaning | Examples |
|------|---------|----------|
| `not_found` | Named resource doesn't exist. | `session_memory get` for a missing key. |
| `no_changelog` | `caller_cwd` doesn't canonicalise to a directory, or no CHANGELOG.md was found under the resolved root. Distinct from `format_mismatch`, which fires only when a *YAML* changelog is discovered but the Keep-a-Changelog writer can't append to it (ANTS-2040). | `changelog_log` against a project with no changelog of any kind. |
| `bullet_not_found` | A `roadmap_log` flip/annotate locator (id / anchor / headline) matched no bullet. Now also fires on a `#### Pass N.M` heading roadmap when a `PASS-N-M` locator matches no pass (ANTS-2126 made those formats writable, so a locate-miss is the same `bullet_not_found` as on GFM). Distinct from `format_mismatch`, which on a pass-headings roadmap is now scoped to `op:"create_section"` only. | `roadmap_log op:"flip" id:"ANTS-9999"` when no such bullet exists. |
| `bullet_ambiguous` | A `roadmap_log` flip/annotate locator (anchor / headline) matched **more than one** bullet; the envelope carries `suggestions[]` + `matched` so the caller can disambiguate. Sibling of `bullet_not_found` for the matched-many case. | `roadmap_log op:"flip" headline:"Fix the bug"` when two bullets share that headline. |
| `symbol_not_found` | `read_region` symbol-mode: no outline symbol matches the requested `symbol` name (within the first 1000 outline symbols). Distinct from `not_found` (the *file* is missing) — the file is fine, the symbol isn't in it (ANTS-2021). | `read_region symbol:"noSuchFn"`. |
| `section_not_found` | `read_region` section-mode: no markdown heading matches the requested `section` (by text, slug, or — ANTS-2234 — a unique short-title prefix). The markdown analogue of `symbol_not_found` (ANTS-2221). | `read_region section:"No Such Heading"`. |
| `section_ambiguous` | `read_region` section-mode: a short-title `section` prefix matched **more than one** heading; the envelope carries `candidates[]` (the qualifying slugs) so the caller can pass a fuller title or the slug. The section analogue of `bullet_ambiguous` (ANTS-2234). | `read_region section:"7. Build"` when two headings begin `7. Build …`. |
| `target_not_found` | `feedback_log` maintainer op: a `heading` (± `heading_line`) target matched no block. `compact_shipped` (ANTS-3421) resolves a `#`/`## ` boundary; `assign_id` (ANTS-3447) resolves a `### ` finding — a locate-miss in either is this code. The feedback-file analogue of `bullet_not_found`. | `feedback_log op:"assign_id" heading:"### No such finding"`. |
| `widget_not_found` | An `objectName` passed to an e2e inject verb (`inject-click` / `inject-key` / `grab-image`) resolved to no widget under the target top-level (`e2eResolveTarget` → `findChild` miss); no event is posted and the app does not crash (ANTS-2049 INV-4). **Scope note:** emitted by socket-only `--e2e` verbs (not MCP-registered tools), which share this MCP code space so the harness can dispatch on `code` uniformly. | `inject-click widget:"nope-xyz"`. |
| `target_ambiguous` | `feedback_log` maintainer op: a bare `heading` matched **more than one** block; the envelope carries `candidates[]` (the colliding 1-based heading lines) so the caller can pass `heading_line`. Emitted by `compact_shipped` (ANTS-3421, `#`/`## ` boundaries) and `assign_id` (ANTS-3447, `### ` findings). The feedback-file analogue of `bullet_ambiguous`. | `feedback_log op:"assign_id" heading:"### Issue #1"` when two findings share that heading. |
| `no_window` | No focused tab when one was needed. | tab-scoped tool with empty `caller_cwd` + no focused tab. |
| `no_project` | Two paths to the same meaning — "there is no project to act on". (1) RcGate: no focused project tab. (2) A root-resolving *write* verb whose `caller_cwd` fails to canonicalise; these deliberately bypass RcGate (they are `m_main`-independent) and so emit this rather than `bad_cwd`. Both are the write-side counterpart of `bad_cwd`: same condition, different verb families — prefer `bad_cwd` for dispatch-lambda reads, `no_project` for these writes. | RcGate-guarded write tools with no project tab focused; `audit_falsepos_log` / `audit_dismiss` / `test_audit_recheck` with an unresolvable `caller_cwd`. (**Not** `test_audit_fold_in` — that returns `bad_path` for the same condition.) |
| `no_lanes` | A reviewer-dispatching tool's partition resolved empty AND no `## Module map` heading exists anywhere (ANTS-1352). Distinct from `module_map_unparseable`, which is the heading-present case. | `indie_review_dispatch` / `indie_review_orchestrate` against a project with no `## Module map (src/)` in `docs/subsystems.md` / CLAUDE.md and no override. |
| `module_map_unparseable` | `indie_review_orchestrate`: a `## Module map` heading IS present but no reviewable lanes could be derived — its bullets aren't the `- <subsystem-name> — <summary>` shape the parser reads, or the named subsystems resolve to no source files (ANTS-3481). A `- <path> — <description>` file list is grouped by top-level directory as a fallback, but only when that yields ≥2 lanes (ANTS-3507); a single-dir file list still lands here. Distinct from `no_lanes` (heading genuinely absent) so a session doesn't wrongly conclude it has no map. Envelope names the resolved source file + the expected bullet shape. | `indie_review_orchestrate` on a CLAUDE.md whose `## Module map` is a `- path — description` list confined to one top-level directory. |
| `ai_not_configured` | The project's AI provider is disabled or unset (ANTS-1352). Envelope hint names `Settings → AI`. | `indie_review_dispatch` when `Config::aiEnabled()` is false OR `Config::aiEndpoint()` is empty. |
| `no_roadmap_loaded` | The roadmap dialog has no roadmap loaded. | roadmap-tied verb during early startup. |
| `no_release_heading` | A fold-in verb found no active release heading to insert its block under (and none was passed via `release_block_heading`). Resolved before ID allocation so a no-op insert can't burn `.roadmap-counter` IDs (ANTS-2201). | `indie_review_fold_in` / `debt_sweep_defer` / `cold_eyes_fold_in` against a roadmap with no in-flight/shipped release heading. |
| `plan_exists` | Conflicting state: a plan file already exists. | `plan_template` told to write over an existing file without `overwrite:true`. |
| `settings_exists` | Conflicting state: a per-project settings file already exists, so a create-only op refuses rather than clobber. Envelope carries `path`. Sibling of `plan_exists` (ANTS-2161). | `project_settings op:"init"` when `<root>/.ants/project.json` is already present (use `op:"set"` to update). |
| `file_changed` | Apply-time mismatch: source bytes drifted between scan and fix. | `debt_sweep_apply_fix` after the file was edited. |
| `not_fixable` | The fix the caller asked for isn't a defined operation here. | `debt_sweep_apply_fix` with an op the engine doesn't implement. |
| `needs_triage` | A bulk un-triaged defer was refused. Envelope carries `total`, `non_auto_fixable`, `threshold`. More than `threshold` (25) findings can't be folded into ROADMAP without `triaged:true` — the guard against dumping raw scan output (real + FP-prone + mechanical) as tracked items (ANTS-3346). Auto-fixability is not a safety signal, so the gate keys on total bulk size. | `debt_sweep_defer` with a large un-reviewed batch. |
| `unrecognised_format` | The file shape isn't one the parser handles. Envelope additionally carries `expected_format[]` and standardised `hint` (ANTS-1463). | `roadmap_query` against a file that isn't ants-v1 or GFM. |
| `format_mismatch` | The file is a *recognised* format the verb can read but can't *write* (so `unrecognised_format`, which keys on zero parsed bullets, doesn't apply). Envelope carries `format`, `path` (the recognised file), and an Edit-fallback `hint` (ANTS-2031 / ANTS-2040). The writer/reader format-parity rule (mcp-tools.md §6a, ANTS-2042) mandates this code over a generic absence code whenever discovery recognises a format the writer can't produce. | `roadmap_log op:"create_section"` against a `#### Pass N.M` heading roadmap (`format:"pass-headings"`) — append / append_batch / flip / flip_batch / annotate now *write* on that format (ANTS-2126), leaving create_section the only refusing op; `changelog_log` against a `data/changelog.yaml` (`format:"yaml"`, ANTS-2040) — readable, but no writer for that format yet. |
| `feature_grouped_section` | `changelog_log`: the `## [Unreleased]` section is *feature-grouped* — its direct `### ` children are dated topic headings (`### <id> — <topic> (<date>)`, newest-first) with `**Bold**` category runs beneath, not flat Keep-a-Changelog `### <category>` blocks. The writer models `### ` as the category slot, so a flat-category insert would land mis-ordered at the section end; refuse and let the caller hand-edit the house style (ANTS-3416). Sibling of `format_mismatch` — the file is readable, but this section's shape isn't one the flat-category writer can act on. Detection needs all three: ≥1 `### ` heading, none a canonical category word, ≥1 flush-left `**Bold**` run. | `changelog_log op:"add"` into a MAME-Curator-style dated-topic Unreleased section (`op:"add_batch"` routes each entry to `skipped[]`). |
| `already_running` | A long-running operation is in flight *for this project root*; refuse rather than queue. Carries `running_since_ms` + `retry_after_ms`. | `audit_run` while a prior call on the same root is still working. |
| `server_busy` | A process-global resource cap is saturated, so the work cannot start *anywhere* — as opposed to `already_running`, which is scoped to one project root. Carries `retry_after_ms`. Use when the bound exists to protect the host (RAM, child processes), not to serialise a per-resource operation. | `audit_run` when `AuditRunner::kMaxConcurrentRuns` (2) sweeps are already running, whatever roots they are on (ANTS-3612). |
| `too_many_jobs` | An async job registry is full of still-running entries, so no handle can be minted. Distinct from `server_busy`: the *work* could run, but there is nowhere to track it; the synchronous path stays available. Carries `retry_after_ms`. | `audit_run async:true` with `kAuditJobsMax` (16) jobs already registered (ANTS-3396). |
| `tools_not_ready` | The detector / engine hasn't finished initialising. | early MCP call against `tool_info` before the registry is built. |
| `reports_dir_unreadable` | `reports_dir` canonicalises but the resolved path doesn't exist, isn't a directory, or the calling user lacks read permission (ANTS-1455). | `test_audit_synthesis_prompt allow_outside_project:true reports_dir:"/no/such/dir"`. |
| `reports_dir_empty` | `reports_dir` is a readable directory containing zero `*.md` **or** `*.json` files at top level (ANTS-1455; `*.json` added by ANTS-1485). | `test_audit_synthesis_prompt` against a dir where the per-chunk reports weren't written (empty workflow). Saves the silent-success failure mode the v1 engine had. |
| `no_git_state` | The project root has no `.git/` directory or `git rev-parse HEAD` returned empty (ANTS-1583). The tool's contract needs git state to be meaningful; refuse rather than emit an envelope with zero coverage. | `roadmap_branch_drift` against a non-git project. |
| `not_cached` | The tool's per-project cache file is absent or fails its schema check; the verb has no recorded data to return (ANTS-1299 / ANTS-1300). Distinct from `not_found` (which is keyed by caller-named resource); this is "the cache itself isn't there yet." | `build_status op:"read"` before any `op:"record"` has populated `<root>/.audit_cache/build.json`; `test_results op:"read"` before the first `op:"record"`. |
| `not_audited` | `last_audit_summary`: the project has no audit artifact to summarise — no `.audit_cache/` history, or none matching the request. The audit family's analogue of `not_cached`; they differ only in which cache is missing, so a caller handling one should handle both. | `last_audit_summary` on a project that has never run `audit_run`. |
| `parse_failed` | An artifact the verb located exists but could not be parsed into the expected shape (truncated, wrong schema, corrupt). Distinct from `not_audited` / `not_cached` (nothing there) — here something is there and is unusable. | `last_audit_summary` against a SARIF file truncated by a killed run. |
| `no_tests_found` | A test-oriented verb resolved its scope but found no test files, or detected no framework. Not an error state for the project, just nothing to act on. | `test_audit_partition` on a repo with no `tests/` and no recognised framework. |
| `no_tools_runnable` | `audit_run`: an explicitly-named `tools[]` entry did not resolve on PATH. Honours caller intent — a named tool that can't run is a refusal, not a silent skip. An **empty** auto-detect sweep is *not* refused even with no external tool present: it proceeds so the in-process drift lanes still run (ANTS-3605). | `audit_run tools:["semgrep"]` on a host without semgrep. |
| `id_counter_failed` | An id-allocating write verb could not read or advance the project's `.roadmap-counter`. The envelope carries `counter_path` so the caller can clear the blockage (ANTS-1527). | `test_audit_fold_in` against a project whose counter file is unwritable. |
| `stale_partition` | A `partition_token` echoed back by the caller is **not present in the engine's in-memory partition cache** (an LRU capped at 16 partitions). Note this is a cache-presence check, *not* a recompute: the token is never re-derived, so a changed file set does **not** invalidate it — the token survives until it is evicted or the process restarts (ANTS-1397 INV-4). | `test_audit_brief` after an Ants restart, or after 16 later partitions evicted the entry. |
| `roadmap_unavailable` | `feedback_log op:"compact_resolved"` (ANTS-3443) could not locate or read a `ROADMAP.md` under the caller project. The op resolves each finding's assigned id to its live status from the roadmap, so an unknowable "is it ✅?" must refuse rather than risk collapsing an un-shipped finding. Distinct from `no_roadmap_loaded` (`roadmap_query`'s active-tab miss) — this is keyed on the `caller_cwd` project. A readable-but-empty roadmap is NOT this refusal (every finding then reads `roadmap_unresolved_ids`, the safe no-op). | `feedback_log op:"compact_resolved"` from a project with no `ROADMAP.md`. |
| `detail_not_found` | The cache exists but the caller's `detail` selector (e.g. a named failing test) is not present in it (ANTS-1300). Distinct from `not_cached` (no cache at all) and `not_found` (resource by path). | `test_results op:"read" detail:"NoSuchTest"` when the recorded `failing_tests[]` doesn't carry that name. |
| `too_large` | A resource exists but is too large for the requested operation. Used at `read_region`/`apply_edits` oversized-file skips and, from ANTS-3545, by `read_spill` **row mode** when the spilled body exceeds `kStructuredParseMaxBytes` (1 MiB) — row-paging must parse the whole body, so an over-cap body is statted and refused before it is read into RAM. Distinct from `result_too_large` (§ 6, a marshalled *output* over a byte cap); `too_large` is an *input resource* over a cap. | `read_spill row_offset:0` against a spilled `get_scrollback` body > 1 MiB — byte-page it (`offset`/`max_bytes`) instead, which does not parse. |
| `not_array` | `read_spill` **row mode** (ANTS-3545): the spilled body is not a JSON object with a non-empty root array member, so it has no rows to page (a scalar-only body, a bare root array, or a fully-tabular `{__cols__,__rows__}` body). The resource exists but isn't the row-shape row mode acts on — byte-page it instead. | `read_spill row_count:10` against a spilled body with no dominant array. |

### 3 — Caller-cwd contract (ANTS-1404 / ANTS-1372)

> **See also:** [ADR-0004 — same-UID trust model](../decisions/0004-same-uid-trust-model-for-mcp-audit-suite.md).
> The caller-cwd contract enforces per-PROJECT isolation under a
> per-UID trust assumption. The ADR documents what the trust
> boundary does and does NOT cover for the audit / synth tool
> suite.

| Code | Meaning | Diagnostic path |
|------|---------|-----------------|
| `caller_cwd_required` | Dispatcher refused: tool is `CallerCwdContract::Required` and `caller_cwd` was empty. | Envelope carries `hint` naming `caller_cwd_info`. **ANTS-1853/2135:** when the *whole* `arguments` object arrived empty (not just `caller_cwd`), the envelope also sets `arguments_empty:true` + size-aware steer text — the call's entire payload was dropped *upstream* in Claude-Code serialisation (the data never reached Ants; not an Ants bug), so resend the whole call / shrink the body / use Edit. The `mcp_trace` `raw_bytes` field confirms it: a small `raw_bytes` with `arg_bytes:2` = body never arrived (upstream); a large `raw_bytes` with `arg_bytes:2` would instead point at an Ants-side parse loss. |
| `tab_or_cwd_required` | Dispatcher refused: tool is `CallerCwdContract::TabSpecific` and no usable routing key was supplied — `caller_cwd` empty AND no integer `tab` (for the two tab-routing tools `get_text` / `recent_errors`). Closes the focused-tab fallback leak (ANTS-1415 Phase 3b). | Envelope carries `hint` naming `caller_cwd_info` + `tab_list`. e.g. `get_text` with neither `tab` nor `caller_cwd`. |
| `cwd_missing` | RcGate refused: tool needs `caller_cwd` and the caller didn't supply one. | Envelope carries `hint` naming `caller_cwd_info`. |
| `cwd_bad` | RcGate refused: `caller_cwd` doesn't canonicalise. | No hint — the caller has a `caller_cwd`, it just doesn't resolve. |
| `cwd_mismatch` | RcGate refused: `caller_cwd` doesn't match the focused-tab cwd (write-side tools that need both to agree, ANTS-1372). | No hint — the caller has a `caller_cwd`, focus on the gate's diagnostic in `error`. |

### 4 — I/O failure

| Code | Meaning | Examples |
|------|---------|----------|
| `read_failed` | File-system read returned an error. | `roadmap_query` against an unreadable ROADMAP.md. |
| `write_failed` | File-system write returned an error. | `roadmap_log` couldn't write the new bullet. |
| `mkdir_failed` | Directory creation returned an error. | engine couldn't create its workspace dir. |
| `io_error` | Generic catch-all for an OS-level I/O failure that doesn't fit one of the above. | Use sparingly — prefer the specific variants when the failing op is known. |

### 5 — Dispatcher / registry

| Code | Meaning | Examples |
|------|---------|----------|
| `unknown_tool` | The dispatcher has no provider for the tool name. | `tools/call` with a typo in `name`. |
| `mcp_disabled` | The Ants MCP integration is toggled off (Settings → General → "Enable Ants MCP integration"); the dispatcher refuses every verb before any handler runs. | Any `tools/call` after the master switch is turned off mid-session (ANTS-1901). |
| `e2e_disabled` | An e2e inject verb (`inject-key` / `inject-click` / `resize-window` / `grab-image`) was called on an instance not launched with `--e2e`; `dispatch()` refuses before the handler runs and posts no event / does no resize or grab (ANTS-2049 INV-1/INV-2). **Scope note:** emitted by socket-only `--e2e` verbs (not MCP-registered tools) — they are unreachable through the agent's MCP toolset by design (spec §5) and share this code space so the harness dispatches on `code`. | `inject-key` over the socket of a plain (non-`--e2e`) instance. |

### 6 — Query execution (ANTS-2093, `project_query`)

Outcomes of running an agent-supplied **sandboxed read-only Lua snippet**
server-side. A new category, not a fold into 1–5: these describe the
*execution* of caller-supplied code (none of input-validation /
resource-state / caller-cwd / I/O / dispatcher covers that), and each is a
genuinely distinct caller action — fix the snippet / make it cheaper /
narrow the data / aggregate harder / re-enable the feature.

| Code | Meaning | Examples |
|------|---------|----------|
| `query_error` | The snippet failed to load or run, or its return value can't be marshalled: a syntax/runtime error or explicit `error(...)`, a `project.*` path that escapes the project root, an unsupported return type (function/userdata/thread), a non-string table key, invalid UTF-8, or a table nested past 32 levels (also how a circular table is caught). The Lua message is carried in `error`. | `project_query` with `code:"return project.read('../../etc/passwd')"` (path escape) or `code:"return function() end"` (unsupported type). |
| `query_timeout` | The snippet exceeded the wall-clock budget (`claude.mcp_project_query_timeout_ms`, default 1500). A pure-Lua/C-in-loop runaway is killed at the next bytecode boundary; a single uninterruptible C call is detached at the join deadline (budget + 250 ms grace). | `project_query` with `code:"while true do end"`. |
| `query_oom` | The snippet tripped the VM's 10 MiB allocation cap (the allocator returns null → Lua raises a memory error). The refusal, never a partial/nil result. | `project_query` with a snippet that grows a table without bound. |
| `result_too_large` | The marshalled result's UTF-8 byte count exceeds `claude.mcp_project_query_result_cap_bytes` (default 64 KiB). Carries the byte size; emits **no** `result` (never truncated). | `project_query` returning every line of every file instead of a count — aggregate harder. |
| `query_disabled` | The `project_query` feature is off (`claude.mcp_project_query_enabled` false, Settings → General → "Let Ants run read-only project queries for Claude"). Checked *first* in the handler, before arg validation; the master `mcp_disabled` gate still takes precedence (it refuses before the handler runs). | Any `project_query` call while the feature toggle is off. |

## Adding a new code

1. Pick the category your refusal belongs to.
2. Check the table — if an existing code's meaning covers your
   case, reuse it. Don't split for stylistic reasons.
3. If you need a new code, add the row + 1-line meaning + at least
   one example in the same commit as the refusal site.
4. Cite this doc in the implementation comment (e.g.
   `// see docs/standards/mcp-error-codes.md § 1`) so a future
   contributor can find the table from the source.
5. Keep names `lowercase_snake_case` and short (≤ 24 chars).
   `bad_path` not `pathValidationFailed`; `caller_cwd_required`
   not `missing_caller_cwd_arg`.

## What this taxonomy is NOT

- **Not a HTTP-status mapping.** MCP responses are JSON envelopes,
  not HTTP. Code values describe the refusal class, not a numeric
  protocol code.
- **Not a stack-trace.** `code` is the *kind* of refusal. The
  `error` field carries the operator-facing message; the `hint`
  field (when present) names the diagnostic path. Keep `code`
  callable-by-machine.
- **Not exhaustive across the JSON-RPC layer.** The JSON-RPC
  transport (claudeintegration.cpp `onMcpConnection`) handles
  protocol-level errors with the JSON-RPC `error.code` integer
  (e.g. `-32601` method-not-found). That layer is upstream of
  this taxonomy; this document covers per-tool refusals only.
