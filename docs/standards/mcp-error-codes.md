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
| `bad_id` | An `id`-typed arg is malformed, or a required id/path locator is wholly absent. | `spec_query`/`spec_log` with an `id` that isn't `ANTS-NNNN` / `phase_<NN>_<topic>`, or with neither `id` nor `path` (ANTS-1309/1963). |
| `bad_path` | A path argument fails NFC / control-char / canonicalisation / project-root anchor (PathValidation::validatePath, ANTS-1295). | `audit_run path:"../../etc"`; symlink-escape. |
| `not_feedback_file` | A `path` arg is well-formed and in-bounds but its resolved basename is not a `*_Ants_MCP_Feedback.md` file (the `feedback_query` / `feedback_log` suffix guard, ANTS-1961/1962). Distinct from `bad_path` (which is a malformed/escaping path) — this path is fine, just the wrong kind of file. | `feedback_query path:"notes.md"`. |
| `bad_cwd` | The caller's `caller_cwd` does not exist or isn't a directory. | `caller_cwd:"/no/such/path"`. |
| `bad_mode` | A mode-enum argument doesn't match any allowed value. | `roadmap_query mode:"foo"`. |
| `bad_mode_combo` | A mode + other-arg combo is conceptually exclusive. | `roadmap_query mode:"section_index"` + `section:"x"`. |
| `bad_section` | A `section` slug isn't in the roadmap's heading index. | `roadmap_query section:"nonexistent"`. |
| `bad_case` | A slug or id locator differs only in case from a real entry; envelope carries the canonical form. | `roadmap_log section:"Performance"` when the slug is `performance`. Returns `canonical_slug:"performance"`. Used by `roadmap_query`, `roadmap_log op:append`, `roadmap_log op:append_batch`, `roadmap_log op:create_section`, and `spec_query`. |
| `bad_status` | A status-typed arg — a query *filter* or a *write value* — doesn't match the enum. | `roadmap_query status:"foo"`; `feedback_log` tracking-row `status` outside the `{📋 🚧 ✅ 💭 🔄 ❓}` set (ANTS-1962). |
| `bad_kind` | A `kind` enum arg doesn't match the recognised set (per roadmap-format.md § 3.5.3). | `roadmap_log kind:"weird"`. |
| `bad_level` | The `level` arg under `roadmap_log op:create_section` is not 2 or 3. | `roadmap_log op:create_section level:5` (ANTS-1878). |
| `bad_intro` | The `intro_body` under `roadmap_log op:create_section` contains a line matching `^#{1,6}\s` (would silently add a heading to the index). | `intro_body:"## stray"` (ANTS-1878). |
| `bad_title` | The `title` arg slugifies to the empty string (all non-letter-or-number characters). | `roadmap_log op:create_section title:"!@#"`. |
| `slug_collision` | The computed slug under `op:create_section` already exists in the index. Envelope carries `computed_slug`. | `roadmap_log op:create_section title:"Performance"` when a `performance` slug exists (ANTS-1878). |
| `headline_empty` | A bullet's `headline` field is empty. | `roadmap_log op:append_batch` bullet with no `headline` field (joins `skipped[]`). |
| `no_roadmap` | `caller_cwd` doesn't canonicalise to a directory, or no ROADMAP.md was found under the resolved root. | `roadmap_log caller_cwd:"/no/such/path"`. Used by every `roadmap_log` op (ANTS-1424, 1878, 1879). |
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
| `no_window` | No focused tab when one was needed. | tab-scoped tool with empty `caller_cwd` + no focused tab. |
| `no_project` | RcGate: no focused project. | All RcGate-guarded write tools when Ants has no project tab focused. |
| `no_lanes` | A reviewer-dispatching tool's partition resolved empty (ANTS-1352). | `indie_review_dispatch` against a project with no `## Module map (src/)` in CLAUDE.md and no override. |
| `ai_not_configured` | The project's AI provider is disabled or unset (ANTS-1352). Envelope hint names `Settings → AI`. | `indie_review_dispatch` when `Config::aiEnabled()` is false OR `Config::aiEndpoint()` is empty. |
| `no_roadmap_loaded` | The roadmap dialog has no roadmap loaded. | roadmap-tied verb during early startup. |
| `plan_exists` | Conflicting state: a plan file already exists. | `plan_template` told to write over an existing file without `overwrite:true`. |
| `settings_exists` | Conflicting state: a per-project settings file already exists, so a create-only op refuses rather than clobber. Envelope carries `path`. Sibling of `plan_exists` (ANTS-2161). | `project_settings op:"init"` when `<root>/.ants/project.json` is already present (use `op:"set"` to update). |
| `file_changed` | Apply-time mismatch: source bytes drifted between scan and fix. | `debt_sweep_apply_fix` after the file was edited. |
| `not_fixable` | The fix the caller asked for isn't a defined operation here. | `debt_sweep_apply_fix` with an op the engine doesn't implement. |
| `unrecognised_format` | The file shape isn't one the parser handles. Envelope additionally carries `expected_format[]` and standardised `hint` (ANTS-1463). | `roadmap_query` against a file that isn't ants-v1 or GFM. |
| `format_mismatch` | The file is a *recognised* format the verb can read but can't *write* (so `unrecognised_format`, which keys on zero parsed bullets, doesn't apply). Envelope carries `format`, `path` (the recognised file), and an Edit-fallback `hint` (ANTS-2031 / ANTS-2040). The writer/reader format-parity rule (mcp-tools.md §6a, ANTS-2042) mandates this code over a generic absence code whenever discovery recognises a format the writer can't produce. | `roadmap_log op:"create_section"` against a `#### Pass N.M` heading roadmap (`format:"pass-headings"`) — append / append_batch / flip / flip_batch / annotate now *write* on that format (ANTS-2126), leaving create_section the only refusing op; `changelog_log` against a `data/changelog.yaml` (`format:"yaml"`, ANTS-2040) — readable, but no writer for that format yet. |
| `already_running` | A long-running operation is in flight; refuse rather than queue. | `audit_run` while a prior call is still working. |
| `tools_not_ready` | The detector / engine hasn't finished initialising. | early MCP call against `tool_info` before the registry is built. |
| `reports_dir_unreadable` | `reports_dir` canonicalises but the resolved path doesn't exist, isn't a directory, or the calling user lacks read permission (ANTS-1455). | `test_audit_synthesis_prompt allow_outside_project:true reports_dir:"/no/such/dir"`. |
| `reports_dir_empty` | `reports_dir` is a readable directory containing zero `*.md` files at top level (ANTS-1455). | `test_audit_synthesis_prompt` against a dir where the per-chunk reports weren't written (empty workflow). Saves the silent-success failure mode the v1 engine had. |
| `no_git_state` | The project root has no `.git/` directory or `git rev-parse HEAD` returned empty (ANTS-1583). The tool's contract needs git state to be meaningful; refuse rather than emit an envelope with zero coverage. | `roadmap_branch_drift` against a non-git project. |
| `not_cached` | The tool's per-project cache file is absent or fails its schema check; the verb has no recorded data to return (ANTS-1299 / ANTS-1300). Distinct from `not_found` (which is keyed by caller-named resource); this is "the cache itself isn't there yet." | `build_status op:"read"` before any `op:"record"` has populated `<root>/.audit_cache/build.json`; `test_results op:"read"` before the first `op:"record"`. |
| `detail_not_found` | The cache exists but the caller's `detail` selector (e.g. a named failing test) is not present in it (ANTS-1300). Distinct from `not_cached` (no cache at all) and `not_found` (resource by path). | `test_results op:"read" detail:"NoSuchTest"` when the recorded `failing_tests[]` doesn't carry that name. |

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
