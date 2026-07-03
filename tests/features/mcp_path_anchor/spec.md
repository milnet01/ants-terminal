# mcp_path_anchor — feature-conformance contract

Locks ANTS-1295: every path-accepting MCP tool routes through
`PathValidation::validatePath` (`src/pathvalidation.{h,cpp}`) so the
cwd-anchor check lives in exactly one place and out-of-root paths
reject uniformly.

Spec source: `docs/specs/ANTS-1295.md`.

## Invariants exercised

- **PV-1 empty rawPath neutral.** `validatePath("", root, "tool")` →
  `Check{bad=false, argvForm="", resolved=""}`. Mirrors INV-8.

- **PV-2 simple inside-root accept.** Relative path that exists
  inside a tmpdir-root canonicalises and returns `bad=false`
  with `resolved == joined-canonical`.

- **PV-3 dash-prefix argvForm.** A valid path beginning with `-`
  returns `bad=false` with `argvForm == "./" + rawPath`. Mirrors
  INV-7.

- **PV-4 relative traversal rejects.** `../../etc/passwd` →
  `bad=true`, `err["code"] == "bad_path"`, `err["error"]` contains
  the tool name, the param name, and "escapes project root".

- **PV-5 absolute outside-root rejects.** `/etc/passwd` →
  `bad=true` with the same envelope shape.

- **PV-6 control character rejects.** A path containing a U+0001
  → `bad=true`, error mentions "control or backslash".

- **PV-7 backslash rejects.** A path containing `\\` → same as PV-6.

- **PV-8 NFC normalisation.** A path expressed in NFD form
  canonicalises identically to its NFC form; both accept.

- **PV-9 non-existent escape rejects (lexical fallback).** A path
  like `nonexistent/../../etc/passwd` where the prefix doesn't
  exist still rejects through `QDir::cleanPath` (INV-5).

- **PV-10 non-existent inside-root accept.** A path like
  `new_dir/new_file` that doesn't exist yet but resolves inside
  root accepts (used by git pathspec for deleted files).

- **PV-13 feedback file allowed outside root (ANTS-3430).** A path
  whose basename ends in `_Ants_MCP_Feedback.md` is permitted to
  escape the project root (the shared corpus lives one level above
  it): an existing such file → `bad=false` with `resolved` set; a
  non-existent one → `bad=false` with `resolved==""`. Supersedes the
  ANTS-3419 refuse-with-hint behaviour; a non-feedback escape (PV-5)
  still refuses and carries no `hint`.

## Wiring (source-grep)

- **WI-1** `src/pathvalidation.h` exists; declares
  `PathValidation::Check` (with `bad`, `err`, `argvForm`,
  `resolved` fields) and `PathValidation::validatePath`.
- **WI-2** `src/pathvalidation.cpp` defines `validatePath` and
  contains the canonical anchor logic (`canonicalFilePath` +
  `startsWith(rootCanonical + '/')`).
- **WI-3** `remotecontrol.cpp` no longer contains an inline
  anchor pair: zero `canonicalFilePath()` calls within five lines
  of a `startsWith(rootCanonical` line. The two remaining
  `startsWith(rootCanonical` occurrences are response-path
  reframing (strip root prefix from result paths), not anchor
  checks.
- **WI-4** `remotecontrol.cpp` contains ≥ 8
  `PathValidation::validatePath(` calls (six MCP tools + the
  resolveLaneFiles defence-in-depth use).
- **WI-5** `cmdWorkspaceSearch` no longer emits `bad_lane` for the
  anchor-failure case; `bad_lane` no longer appears in the function
  at all after the rewrite.
- **WI-6** `CMakeLists.txt` lists `src/pathvalidation.cpp` in
  `ants_core_lib`.

## Pre-fix regression check

Engine tests fail to link against pre-fix code (the module doesn't
exist). Wiring tests fail because:
- `pathvalidation.h` is absent (WI-1, WI-2).
- The inline anchor pattern still exists in remotecontrol.cpp (WI-3).
- The number of `PathValidation::validatePath(` call-sites is zero
  pre-fix (WI-4).
- `bad_lane` appears in cmdWorkspaceSearch's anchor block (WI-5).

All failures are loud.
