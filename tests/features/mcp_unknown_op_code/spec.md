# An unknown op is refused with `bad_mode` on every multi-op verb

A verb that takes an `op` answers an `op` it does not know with the code
`bad_mode` (ANTS-6771, user decision 2026-11-15). Before, the same mistake
drew `bad_op`, `bad_op_combo`, `bad_args` or `bad_mode`, depending on the
verb, so a caller could not handle it one way.
`docs/standards/mcp-error-codes.md` holds the code's row.

## Invariants

- **INV-1 — one code.** Each verb below, called through `ants-mcpd` with
  `op:"no_such_op"` and a `caller_cwd`, refuses with `ok:false` and
  `code:"bad_mode"`: `build_status`, `changelog_log`, `ci_report`,
  `feedback_log`, `git_state`, `know_how`, `project_settings`,
  `roadmap_log`, `roadmap_migrate`, `run_trace`, `session_memory`,
  `session_message`, `spec_log`, `subsystem`, `test_results`,
  `workflow_state`.

`roadmap_migrate` checks its `op` before anything else: before ANTS-6771 an
`op` other than `deregister`, `reroot` or `init` ran the default migration.
The test cannot show that, since the verb refuses a project under the
temp dir; it shows the code.

## Test

`test_mcp_unknown_op_code.cpp` in the `test_claude` bundle drives a real
`ants-mcpd` child (`McpdSession`) against a fresh project directory. INV-1
fails against the code before the change for every verb that answered
another code.
