# caller_cwd_info_verb — ANTS-1400

See `docs/specs/ANTS-1400.md`.

## Test scope

Source-scrape regression locks the `caller_cwd_info` MCP verb
registration, schema, classification, and handler delegation.

## Invariants checked

- **REG-1.** `caller_cwd_info` registered as an MCP tool in
  `mainwindow.cpp` via `registerToolProvider`.
- **REG-2.** Provider lambda delegates to `ants::resolveCallerCwdRoot`
  and emits the four envelope fields (`ok`, `source`,
  `resolved_cwd`, `tab_index`).
- **REG-3.** Schema block in `claudeintegration.cpp` lists the
  verb with `caller_cwd` property and an EMPTY `required` array.
- **REG-4.** `callerCwdContractFor` classifies the verb as
  `Optional` (not Required — empty caller_cwd is a legitimate
  input that exercises the EmptyFallback case).
- **REG-5.** `sourceToString` helper enumerates all four
  `ResolvedRoot::Source` values.
