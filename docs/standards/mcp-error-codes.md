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
| `bad_args` | A required argument is missing or has the wrong shape. | `cmdRoadmapLog` without `headline`; numeric arg parsed as non-numeric. |
| `bad_path` | A path argument fails NFC / control-char / canonicalisation / project-root anchor (PathValidation::validatePath, ANTS-1295). | `audit_run path:"../../etc"`; symlink-escape. |
| `bad_cwd` | The caller's `caller_cwd` does not exist or isn't a directory. | `caller_cwd:"/no/such/path"`. |
| `bad_mode` | A mode-enum argument doesn't match any allowed value. | `roadmap_query mode:"foo"`. |
| `bad_mode_combo` | A mode + other-arg combo is conceptually exclusive. | `roadmap_query mode:"section_index"` + `section:"x"`. |
| `bad_section` | A `section` slug isn't in the roadmap's heading index. | `roadmap_query section:"nonexistent"`. |
| `bad_status` | A status-filter arg doesn't match the enum. | `roadmap_query status:"foo"`. |
| `bad_feature_name` | A `feature_name` arg doesn't match the allowed pattern. | `plan_template feature_name:"!!!"`. |
| `missing_name` | A name-typed required arg is empty. | `tool_info name:""`. |
| `rate_limited` | The caller exceeded the per-tool sliding-window cap (ANTS-1356). The envelope carries `retry_after_ms`. | `audit_run` 11th call within 60 s (Expensive tier cap = 10/min). Caller should honour `retry_after_ms` before retrying. |

### 2 — Resource state (the requested object isn't where the tool can act on it)

| Code | Meaning | Examples |
|------|---------|----------|
| `not_found` | Named resource doesn't exist. | `session_memory get` for a missing key. |
| `no_window` | No focused tab when one was needed. | tab-scoped tool with empty `caller_cwd` + no focused tab. |
| `no_project` | RcGate: no focused project. | All RcGate-guarded write tools when Ants has no project tab focused. |
| `no_roadmap_loaded` | The roadmap dialog has no roadmap loaded. | roadmap-tied verb during early startup. |
| `plan_exists` | Conflicting state: a plan file already exists. | `plan_template` told to write over an existing file without `overwrite:true`. |
| `file_changed` | Apply-time mismatch: source bytes drifted between scan and fix. | `debt_sweep_apply_fix` after the file was edited. |
| `not_fixable` | The fix the caller asked for isn't a defined operation here. | `debt_sweep_apply_fix` with an op the engine doesn't implement. |
| `unrecognised_format` | The file shape isn't one the parser handles. | `roadmap_query` against a file that isn't ants-v1 or GFM. |
| `already_running` | A long-running operation is in flight; refuse rather than queue. | `audit_run` while a prior call is still working. |
| `tools_not_ready` | The detector / engine hasn't finished initialising. | early MCP call against `tool_info` before the registry is built. |

### 3 — Caller-cwd contract (ANTS-1404 / ANTS-1372)

> **See also:** [ADR-0004 — same-UID trust model](../decisions/0004-same-uid-trust-model-for-mcp-audit-suite.md).
> The caller-cwd contract enforces per-PROJECT isolation under a
> per-UID trust assumption. The ADR documents what the trust
> boundary does and does NOT cover for the audit / synth tool
> suite.



| Code | Meaning | Diagnostic path |
|------|---------|-----------------|
| `caller_cwd_required` | Dispatcher refused: tool is `CallerCwdContract::Required` and `caller_cwd` was empty. | Envelope carries `hint` naming `caller_cwd_info`. |
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
