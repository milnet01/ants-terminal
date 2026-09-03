# Feature test — `find_definition` / `find_caller` MCP symbol queries (ANTS-1303)

Contract for the `SymbolQuery` Core lib + its MCP/IPC wiring. Full
design: `docs/specs/ANTS-1303.md`.

## What this test locks

**Live `SymbolQuery` behaviour** (against a synthetic `QTemporaryDir`
source tree spanning C++, Python, Lua, Shell):

1. `findDefinition` returns the definition site, ordered definition
   before declaration, with `kind` per ANTS-1303 INV-8 — a line ending
   in `;` that opens no body is a `declaration`. A brace opens a body
   only after a parameter list, a capture list or a class-key, so
   `QTimer *m_t{nullptr};` is a declaration (ANTS-4821).
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
8a. (ANTS-1700) A namespace-qualified *call* site (`ns::sym(`) is not
    mis-classified as a definition: the C++ def anchor requires a
    return-type token before the (optionally qualified) name. The real
    qualified definition `QByteArray ns::slurpBody(...)` is still found
    (`definitionsTotal == 1`), and no call line appears in
    `definitions`.
8b. (ANTS-2146) A bare call in statement position (`return slurpBody(p);`)
    is not mis-tagged as a `declaration`: the C++ def anchor rejects an
    expression-introducing reserved keyword (`return`/`co_return`/
    `co_await`/`co_yield`/`throw`/`else`) as the leading return-type token.
    `definitionsTotal` stays 1 and no emitted signature begins with
    `return `.

**Wiring contract** (source-grep over the four wiring files):

9.  `remotecontrol.h` declares `cmdFindDefinition` / `cmdFindCaller`;
    `remotecontrol.cpp` defines them and dispatches the
    `find-definition` / `find-caller` IPC verbs.
10. `mainwindow.cpp` registers both tools via `registerToolProvider`
    with the `Required` contract.
11. `claudeintegration.cpp` carries both tool descriptors, the
    token-cost entries, the `"symbol"` `kindForName` bucket, and the
    `Required` `callerCwdContractFor` branches.

**`find_caller` files_only manifest** (ANTS-3555, source-grep — cmdFindCaller
has no public test seam, same contract as INV-9/10/11):

12. `cmdFindCaller` parses a `files_only` arg and, when set, early-returns a
    manifest — `{files:[{file, count, lines[]}], files_count, callers_count,
    files_only:true, definition?, …}` — that drops the quoted per-call
    `context` windows. The branch precedes the full `callers[]` loop (so the
    context lines are never built), and `count` equals the per-file `lines[]`
    length (the returned, possibly `max_results`-capped, call sites), while
    top-level `callers_count` still carries the true pre-cap total.
13. The `find_caller` `tools/list` descriptor enumerates `files_only`.

Exit 0 = every invariant holds.
