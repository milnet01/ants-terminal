# Two names for one argument refuse (ANTS-6365)

`docs/standards/mcp-tools.md`'s schema-hygiene rule: when a caller sends an
argument and its alias together, the verb refuses `bad_op_combo` and lists
every name it was sent in `supplied[]`, canonical name first.

## Invariants

- **INV-1** — `roadmap_query`: `status`+`filter`, `query`+`q`,
  `limit`+`max_results`.
- **INV-2** — `workspace_search`: `pattern`+`query`.
- **INV-3** — `file_outline`: `path`+`file_path`. `codebase_index`:
  `file_path`+`path`.
- **INV-4** — `co_change_family`: `stems`+`stem`. `find_sources`:
  `topic`+`symbol`.
- **INV-5** — `apply_edits`, per edit: any two of `old`, `old_string`,
  `old_text`, or of `new`, `new_string`, `new_text`.

## Test

`test_mcp_alias_clash.cpp` calls each handler with both names and checks
the refusal code and `supplied[]`.
