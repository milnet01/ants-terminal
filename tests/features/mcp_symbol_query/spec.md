# Feature test — `find_definition` / `find_caller` MCP symbol queries (ANTS-1303)

Contract for the `SymbolQuery` Core lib + its MCP/IPC wiring. Full
design: `docs/specs/ANTS-1303.md`.

## What this test locks

**Live `SymbolQuery` behaviour** (against a synthetic `QTemporaryDir`
source tree spanning C++, Python, Lua, Shell):

1. `findDefinition` returns the definition site, ordered definition
   before declaration, with `kind` keyed off a trailing `;`
   (header decl → `declaration`, body def → `definition`).
2. `findCaller` returns call sites, excludes the definition line
   (INV-9), and attaches the best-guess `definition`.
3. Per-language anchors fire for C++ / Python / Lua / Shell.
4. The tree walk skips `build*` / dot-dirs / `node_modules`.
5. An explicit `lang` filter restricts the scan to one family.
6. Emitted `file` paths are project-relative (root prefix stripped).
7. `maxResults` caps the array; `*_count` carries the pre-cap total
   and `truncated` flips.
8. `isValidSymbol` accepts identifiers and rejects empties, leading
   digits, regex metachars, and >128-char input.

**Wiring contract** (source-grep over the four wiring files):

9.  `remotecontrol.h` declares `cmdFindDefinition` / `cmdFindCaller`;
    `remotecontrol.cpp` defines them and dispatches the
    `find-definition` / `find-caller` IPC verbs.
10. `mainwindow.cpp` registers both tools via `registerToolProvider`
    with the `Required` contract.
11. `claudeintegration.cpp` carries both tool descriptors, the
    token-cost entries, the `"symbol"` `kindForName` bucket, and the
    `Required` `callerCwdContractFor` branches.

Exit 0 = every invariant holds.
