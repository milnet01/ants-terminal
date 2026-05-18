# MCP / IPC error-code conventions

> **Superseded by [mcp-error-codes.md](mcp-error-codes.md) (ANTS-1353).**
> This file is the earlier (2026-05-12) draft from the
> optimisation-lane cold-eyes pass; the authoritative taxonomy
> now lives at `mcp-error-codes.md`. Kept as historical
> reference only — do not author new error codes against this
> document.

Source: cold-eyes pass 2 on the token-reduction spec pack
(ANTS-1247..1252), 2026-05-12. Optimisation-lane reviewer M1/M2.

All MCP tools and IPC verbs that share the `{ok, error, code}`
envelope (introduced in ANTS-1117) follow these families. Each new
error code must fit one of them or motivate a new family in this
doc — not invent a one-off shape.

## Families

### `bad_<arg>` — input validation

Caller passed a malformed / out-of-range argument. Code names the
*argument*, not the failure mode.

Examples:
- `bad_pattern` (empty / null pattern; ANTS-1248)
- `bad_lane` (path-escape / unknown encoding; ANTS-1248)
- `bad_path` (canonical-startswith failed; ANTS-1249, 1250)
- `bad_range` (rev-range regex failed; ANTS-1250)
- `bad_status` (status filter not in enum; ANTS-1247)
- `bad_glob` (glob too long / contains `..`; ANTS-1248)
- `bad_op` (op discriminator not in enum; ANTS-1250, 1251)

### `<thing>_failed` — tool/external error

An external dependency (rg, git, jq) ran but exited non-zero, or
produced unparseable output.

Examples:
- `read_failed` (file open/read failed; ANTS-1117)
- `rg_failed` (ripgrep non-zero exit; ANTS-1248)
- `git_failed` (git non-zero exit; ANTS-1250)
- `parse_failed` (output couldn't be parsed; reserved)

Response includes a `stderr:"…"` field (capped at 4 KiB) when the
external tool produced one.

### `<thing>_missing` — absent dependency

A required external dependency isn't on the host.

Examples:
- `git_missing` (git not on PATH; ANTS-1250)
- `rg_missing` (reserved; tools/list omits the tool instead)

`tools/list` typically omits the tool entirely when a hard dep is
absent at server start — this code is for late detection (PATH
changed mid-session, etc.).

### `not_<thing>` — state mismatch

The system is in a state that doesn't permit the operation.

Examples:
- `not_git_repo` (cmdGitState run outside a git checkout; ANTS-1250)
- `not_found` (file does not exist; ANTS-1249)

Note: existing pre-1247 codes that don't fit cleanly stay grandfathered:
- `no_roadmap_loaded` (ANTS-1117) — predates this convention; would
  become `not_roadmap_loaded` if rewritten, but the existing surface
  is a contract.

### `unknown_<thing>` — value not in a known set

Distinct from `bad_<arg>`: the argument was *well-formed* but not
in the expected universe (e.g. lane name doesn't appear in CLAUDE.md
Module map).

Examples:
- `unknown_lane` (ANTS-1251)
- `unknown_op` (reserved synonym for `bad_op` when the field is
  optional; prefer `bad_op` for required fields)

## Cross-cutting fields

- `ok: false` always paired with `code: "..."` (non-empty) and
  `error: "..."` (human-readable).
- `<verbatim>` substrings in `error` (echo of attacker-controlled
  input) are capped at 64 bytes and bytes < 0x20 are replaced with
  `?`.
- `stderr: "..."` is set only by `<thing>_failed` codes and is
  capped at 4 KiB.

## Why this matters

Cold-eyes optimisation lane M1: pre-1247 specs invented codes ad
hoc (`unknown_lane` vs `bad_lane` for the same concept). A canonical
list (a) keeps the surface uniform for the model consuming it, (b)
makes future error-handling logic generalisable, (c) prevents a
later spec from needing yet another one-off.

Note: the test convention that originally accompanied this draft
(a `tests/features/mcp_error_codes/` fixture asserting every code
returned by every tool falls into a canonical family) was never
implemented and is not part of the v1 contract. The authoritative
ANTS-1353 standard at
[`mcp-error-codes.md`](mcp-error-codes.md) deferred mechanical
enforcement to a future ANTS-NNNN; see that doc for the current
status.
