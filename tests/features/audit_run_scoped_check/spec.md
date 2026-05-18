# audit_run scoped-check mode (ANTS-1512)

`audit_run`'s full-pipeline shape works well for the "everything across
the whole tree" use case but is the wrong tool for the "one rule across
N subdirs" pattern that shows up in Tier-3 tidy follow-ups (e.g.
RetroArch Bundle 64's `bugprone-integer-division` sweep across
8 menu+gfx files). Previously the caller had to fall back to a raw
`Bash clang-tidy --checks='-*,...'` invocation — losing the structured
warnings, MCP caching, and the in-flight gate.

This spec covers the `paths` + `checks` parameters added by ANTS-1512
that let the caller scope an audit_run invocation down to a specific
check-and-subdir matrix.

## Surface

- `src/auditrunner.h` — `RunRequest` carries `paths` and `checks`
  QStringLists.
- `src/auditrunner.cpp`:
  - `kKnownTools()` now includes `clang-tidy`.
  - `toolHonoursChecks()` predicate returns true only for clang-tidy
    (the only tool with a generalisable `--checks=` axis).
  - `isAuditCheckSafe()` regex (`^-?[A-Za-z0-9_*.,-]+$`, length ≤ 128)
    sanitises every check ID — same fail-safe shape as the existing
    `isAuditArgSafe()` argv guard.
  - `runAudit()` validates `paths` (via `isAuditArgSafe`) and `checks`
    (via `isAuditCheckSafe`). Any unsafe entry rejects the whole call
    with `code:"bad_args"`. When `checks` is non-empty and the
    requested tool is not in `toolHonoursChecks`, the call is also
    rejected with `bad_args` rather than silently ignored.
  - `toolArgv()` accepts `scopedPaths` + `scopedChecks` parameters.
    For `clang-tidy`, scoped checks render as
    `--checks=-*,<comma-joined>`; for both `clang-tidy` and `cppcheck`,
    scoped paths append as positional args. When either is present,
    the project's `.audit-config.json` override path is bypassed
    (scoped invocations are narrow-on-purpose; the project-wide
    override would re-broaden the scope).
- `src/mainwindow.cpp` `audit_run` provider — extracts `paths` and
  `checks` from the args object.
- `src/claudeintegration.cpp` `audit_run` descriptor — declares `paths`
  and `checks` array properties + updates the `tools` description to
  mention `clang-tidy`.

## Invariants

- **INV-1** `src/auditrunner.h` declares `paths` AND `checks`
  QStringList fields on `RunRequest`.
- **INV-2** `src/auditrunner.cpp` `kKnownTools()` includes the
  literal string `"clang-tidy"`.
- **INV-3** `src/auditrunner.cpp` declares `toolHonoursChecks(const
  QString &)` and the body returns true for `"clang-tidy"`.
- **INV-4** `src/auditrunner.cpp` declares `isAuditCheckSafe(const
  QString &)` with an explicit `^-?[A-Za-z0-9_*.,-]+$` regex literal
  (the per-check sanitiser).
- **INV-5** `runAudit()` walks `req.checks`, rejecting with
  `code:"bad_args"` when any entry fails `isAuditCheckSafe` OR when
  any requested tool is not in `toolHonoursChecks`.
- **INV-6** `runAudit()` walks `req.paths`, rejecting with
  `code:"bad_args"` when any entry fails `isAuditArgSafe`.
- **INV-7** `toolArgv()` accepts `scopedPaths` + `scopedChecks`
  parameters; the `clang-tidy` branch renders
  `--checks=-*,<scopedChecks joined with ','>` when the list is
  non-empty.
- **INV-8** `src/mainwindow.cpp` `audit_run` provider extracts `paths`
  AND `checks` arrays from the args object.
- **INV-9** `src/claudeintegration.cpp` `audit_run` descriptor
  declares `paths` and `checks` properties.

## Rationale

Source-grep test — the live runAudit() integration is exercised in
the existing `mcp_audit_run` test; this spec locks down the new
parameter wiring + sanitiser shape so a refactor that drops the
gates surfaces in CI immediately.
