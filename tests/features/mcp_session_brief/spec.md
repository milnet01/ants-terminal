# mcp_session_brief — Feature Spec (ANTS-1724)

## Purpose
`session_brief` returns a compact project-state envelope in one MCP call.
Designed for fresh /clear session orientation.

## Invariants

- **INV-1** Response contains `ok:true`, `git`, `build`, `test`, `audit`,
  `roadmap` keys when the project root resolves.
- **INV-2** `git.branch` is a non-empty string when git is available;
  empty string when the project root has no `.git`.
- **INV-3** `build.result` is one of `"pass"`, `"fail"`, `"unknown"`.
- **INV-4** `test.result` is one of `"pass"`, `"fail"`, `"unknown"`.
- **INV-5** `audit.open_count` is `error + warning + note` from the
  last audit run, or 0 when the cache is absent.
- **INV-6** `roadmap.active_id` is empty string when no 🚧/📋 bullet
  exists in the roadmap.
- **INV-7** When `caller_cwd` is absent or unresolvable the tool refuses
  with `{ok:false, code:"no_project"}`. (See mcp-error-codes.md §input-validation.)
- **INV-8** The serialised JSON response (before MCP wrapping) is ≤ 512
  bytes for a project with ≤ 10 changed files.
- **INV-9** The tool is ETag-eligible (registered in `isEtagSupportedTool`).
- **INV-10** `build.recorded_at` and `test.recorded_at` are ISO-8601
  strings when the cache exists, absent when the cache is missing.
- **INV-11** The refusal envelope on unresolvable cwd carries
  `code:"no_project"` — verified by source-grep in the test.
