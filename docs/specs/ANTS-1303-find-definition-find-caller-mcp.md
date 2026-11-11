# ANTS-1303 — `find_definition` + `find_caller` MCP — cheap symbol queries without full LSP

**Status:** spec draft (2026-05-20).
**Kind:** implement.
**Source:** indie-review-2026-05-13 (roadmap bundle plan pull 35).

Two new read-only MCP tools (plus matching IPC verbs) that answer the
two most-frequent code-navigation questions — "where is `Foo`
defined?" and "what calls `Foo`?" — in one call instead of the 4–6
grep + Read cycles those questions cost today. A new Qt6::Core-only
`SymbolQuery` lib does a bounded recursive regex scan over the
project's source tree; the MCP/IPC layer is a thin delegate.

## 1. Problem

Asking "where is `cmdSpecQuery` defined" or "who calls
`resolveRootCanonical`" currently means:

1. `grep -rn "cmdSpecQuery" src/` → page of hits (def + decl + every
   call site interleaved).
2. Read each candidate file around the hit to tell the definition
   from the call sites.

That is 4–6 tool calls and ~6–10 K of grep + Read tokens per
question. `workspace_search` (ANTS-1248) already does the grep half,
and `grep_context` (ANTS-1304) adds ±N context lines, but neither
separates *the definition* from *the callers* — the structural
distinction that makes the answer useful. `file_outline` (ANTS-1249)
extracts a single file's symbols but cannot answer "across the whole
tree, where is this one symbol."

## 2. Goal

Two tools, both `{symbol, caller_cwd, lang?, max_results?}` in:

- **`find_definition`** → the line(s) where `symbol` is defined or
  declared, structured as `definitions:[{file, line, signature, lang,
  kind}]`.
- **`find_caller`** → the line(s) that *call* `symbol`, structured as
  `callers:[{file, line, context, lang}]`, plus the best-guess
  `definition` (so the caller gets both halves of "where is it and who
  uses it" in a single round trip).

Regex-anchored, no clangd / tree-sitter / build-system dependency.
C++ / Python / Lua / Shell aware. Pairs with `file_outline`; together
they cover ~80 % of LSP without the build overhead (Karpathy § 2 — the
6-regex set covers the 90 % case).

## 3. Surface

### 3.1 New lib — `src/symbolquery.{h,cpp}` (Qt6::Core only)

`namespace SymbolQuery`. No GUI deps; lives in `ants_core_lib` next to
`fileoutline.cpp` (its single-file sibling). Public API:

```cpp
enum class Lang { Auto, Cpp, Py, Lua, Sh };
Lang parseLang(const QString &s);          // "" / "auto" → Auto

// Identifier guard: ^[A-Za-z_][A-Za-z0-9_]{0,127}$.
bool isValidSymbol(const QString &symbol);

struct DefMatch  { QString file; int line; QString signature; QString lang; QString kind; };
struct CallMatch { QString file; int line; QString context;   QString lang; };

// maxResults / maxFiles == 0 is the sentinel "use the tool-specific
// default" (NOT "cap at zero / return nothing"). Defaults in § 3.4.
struct Options { int maxResults = 0; int maxFiles = 0; Lang lang = Lang::Auto; };

struct DefResult {
    bool ok = true; QString error, code;
    QVector<DefMatch> definitions;
    int definitionsTotal = 0; int filesScanned = 0;
    bool truncated = false; bool walkCapped = false;
};
struct CallResult {
    bool ok = true; QString error, code;
    QVector<CallMatch> callers; std::optional<DefMatch> definition;
    int callersTotal = 0; int filesScanned = 0;
    bool truncated = false; bool walkCapped = false;
};

DefResult  findDefinition(const QString &rootCanonical, const QString &symbol, const Options &opts);
CallResult findCaller    (const QString &rootCanonical, const QString &symbol, const Options &opts);
```

The lib takes an already-canonical project root (the MCP/IPC caller
resolves + path-anchors it); the lib never escapes that subtree.

### 3.2 Tree walk

`QDirIterator(root, Files | Readable | NoDotAndDotDot, Subdirectories)`
from `rootCanonical`. Symlinks are NOT followed (QDirIterator default),
so the walk cannot escape the root subtree. Skip rules, applied to
each path's directory components:

- Any component beginning with `.` (`.git`, `.audit_cache`, `.cache`,
  `.indie-review`, `.claude`, …).
- A component named `build` or beginning with `build-`
  (`build`, `build-fast`, `build-asan`, `build-workstation`).
- A component named `node_modules`.

Included files, by extension (lowercased):

| Lang | Extensions |
|------|-----------|
| Cpp  | `cpp cc cxx c h hpp hh hxx` |
| Py   | `py pyi` |
| Lua  | `lua` |
| Sh   | `sh bash` |

(The Cpp set deliberately extends `fileoutline.cpp`'s `cpp/cc/cxx/h/hpp/hh`
with `c` and `hxx` so plain C sources and `.hxx` headers are covered —
a wider net is correct for a tree-wide symbol query than for a
single-file outline.)

In `Auto` (`opts.lang == Lang::Auto`) all four families are scanned;
with an explicit `opts.lang`, only that family's files. Each emitted
`DefMatch.file` / `CallMatch.file` is project-relative — the lib strips
the `rootCanonical + "/"` prefix before returning, so the
MCP/IPC layer serialises relative paths verbatim (INV-13). Files are
read line-by-line via
`QFile::readLine()`; lines longer than 1024 bytes are skipped for
regex matching but still advance the line counter (catastrophic-
backtracking guard, mirrors `fileoutline` INV-8). The walk stops scanning new files once
`filesScanned` reaches `maxFiles` (default 5000) and sets
`walkCapped:true`.

### 3.3 Anchors

The validated `symbol` is `QRegularExpression::escape`d before being
spliced into the per-language anchors below (defence-in-depth on top
of the `isValidSymbol` guard). Because the symbol is dynamic, the
regexes are compiled once **per call** (not process-static like
`fileoutline.cpp`'s symbol-free set) and reused across every file
scanned in that call; `.optimize()` (PCRE2 JIT) plus the 1024-byte
per-line cap bound worst-case backtracking.

Definition anchors (`S` = escaped symbol):

| Lang | Anchor | Notes |
|------|--------|-------|
| Cpp  | `^[\w:&*<>~ \t]*\bS\s*\(` | catches `void Foo::S(`, `QString S(`, `S::S(` ctor, `~S(`; excludes qualified calls (`obj->S(` / `obj.S(` / `x = S(` — `.`/`->`/`=` are not in the prefix class). The `kind` split is INV-8. |
| Py   | `^\s*(?:async\s+)?(?:def\|class)\s+S\b` | `kind="definition"`. |
| Lua  | `^\s*(?:local\s+)?function\s+(?:[\w.:]*[.:])?S\s*\(` OR `^\s*(?:local\s+)?S\s*=\s*function\b` | `kind="definition"`. |
| Sh   | `^\s*(?:function\s+)?S\s*\(\s*\)` OR `^\s*function\s+S\b` | `kind="definition"`. |

Caller anchors:

| Lang | Anchor | Notes |
|------|--------|-------|
| Cpp / Py / Lua | `\bS\s*\(` anywhere on the line | a line that also matches this language's *definition* anchor is NOT reported as a caller (the def line is not its own call site). |
| Sh   | `\bS\b` (word boundary, no call-paren) | shell call syntax is `S arg…`, so the paren convention does not apply; coarser by design — also matches `$S` variable refs and `S=…` assignments, which are NOT filtered in v1. |

**Known textual false-positives (v1, documented not fixed):**

- A bare free-function *call* at statement position (`S();` with no
  qualifier) matches the Cpp definition anchor and, ending in `;`,
  surfaces in `find_definition` as `kind="declaration"`. Regex alone
  cannot tell it from a forward declaration; the `kind` split keeps it
  out of the `definition` bucket.
- CRLF line endings are stripped (trailing `\r`/`\n` chopped) before
  matching, mirroring `fileoutline.cpp`'s read loop, so the `\(`- and
  `\b`-terminated anchors match on Windows-checkout files too.
- Semantic resolution (overloads, scope, macros) is out of scope (§ 6).

`findDefinitions` (ANTS-3680) resolves a list of symbols in one walk;
INV-20 states its equivalence to the per-symbol call. Use it for any
sweep that resolves a document's worth of names — the walk is the same
walk every time, so per-needle re-walking made cost linear in needles.

`find_caller` also runs the definition scan and attaches the
single best `DefMatch` (first `kind="definition"`, else first
`kind="declaration"`) as the result's `definition`; absent when no
definition was found.

### 3.4 Result ordering, caps, RAM

- `find_definition`: definitions ordered `definition` before
  `declaration`, then by (file, line). `maxResults` default 50.
- `find_caller`: callers ordered by (file, line). `maxResults`
  default 200.
- `*Total` fields carry the pre-cap count; `truncated:true` when the
  cap dropped entries. `definitionsTotal` / `callersTotal` always
  reflect the full count.
- Memory is O(results) + one streamed line at a time. Peak working set
  is bounded by `maxResults × (path + signature ≤ ~1 KiB)` plus the
  single largest scanned line (≤ 1 KiB after the per-line cap) — well
  under 1 MiB for the default caps. No persistent cache, no index:
  each call is a fresh streamed walk (timing budget in § 5; the
  `maxFiles` cap bounds pathological monorepos).

### 3.5 MCP / IPC wiring

- `RemoteControl::cmdFindDefinition(const QJsonObject&)` and
  `cmdFindCaller(const QJsonObject&)` declared public in
  `remotecontrol.h` next to `cmdFileOutline`; defined in
  `remotecontrol.cpp` with `ANTS-1303` anchor comments. Each:
  validates `symbol` (refuse `bad_args`), resolves the root via
  `resolveRootCanonical(m_main, req)` (refuse `no_project` when
  empty), reads optional `lang` / `max_results`, calls the lib, and
  serialises the struct to the envelope in § 3.6.
- Both are exposed as IPC verbs in `dispatch()` (siblings to
  `file_outline` / `workspace_search`) AND as MCP tools.
- `MainWindow::setupClaudeMcpProviders` registers `find_definition`
  and `find_caller` via `registerToolProvider` with
  `CallerCwdContract::Required`, delegating to the `cmd*` methods,
  with the `kRcUnavailable` fallback when `m_remoteControl` is null.

### 3.6 Envelopes

Struct-field → wire-key mapping (the only non-1:1 renames): struct
`definitionsTotal` → JSON `definitions_count`; struct `callersTotal` →
JSON `callers_count`; struct `walkCapped` → JSON `walk_capped`;
`filesScanned` → `files_scanned`. All others lower-snake-case 1:1.

`find_definition` success:

```json
{ "ok": true, "symbol": "cmdSpecQuery", "lang": "auto",
  "definitions": [
    {"file":"src/remotecontrol.cpp","line":5268,
     "signature":"QJsonDocument RemoteControl::cmdSpecQuery(const QJsonObject &req) {",
     "lang":"cpp","kind":"definition"} ],
  "definitions_count": 1, "files_scanned": 312,
  "truncated": false, "walk_capped": false }
```

`find_caller` success:

```json
{ "ok": true, "symbol": "resolveRootCanonical", "lang": "auto",
  "callers": [
    {"file":"src/remotecontrol.cpp","line":5280,
     "context":"const QString rootCanonical = resolveRootCanonical(m_main, req);",
     "lang":"cpp"} ],
  "callers_count": 21,
  "definition": {"file":"src/remotecontrol.cpp","line":3661,
     "signature":"QString resolveRootCanonical(MainWindow *main, const QJsonObject &req) {",
     "lang":"cpp","kind":"definition"},
  "files_scanned": 312, "truncated": false, "walk_capped": false }
```

Empty result is `ok:true` with an empty array (a symbol with no
definition / no callers is a valid answer, not an error).

### 3.7 Refusal codes

| code | when |
|------|------|
| `bad_args` | `symbol` missing, empty, or not matching `^[A-Za-z_][A-Za-z0-9_]{0,127}$`; or `max_results` present but not a positive integer. (Reuses the existing input-validation code per `mcp-error-codes.md` § 1 — a malformed required arg, no new code warranted.) |
| `no_project` | root unresolved (no `caller_cwd`, no focused tab) |

(`caller_cwd_required` is emitted upstream by the dispatcher per
ANTS-1404 before the handler runs, when `caller_cwd` is absent.)

### 3.8 Edge cases

| Input | Behaviour |
|-------|-----------|
| Symbol with no definition / no callers | `ok:true`, empty array, `*_count:0` — a valid answer, not an error. |
| Symbol appearing > `maxResults` times | array capped at `maxResults`, `truncated:true`, `*_count` = full pre-cap total. |
| Symbol that is a language keyword (`class`, `def`, `if`) | accepted by `isValidSymbol` (it is a valid identifier shape) and scanned as-is; the caller is responsible for not asking nonsense. No special-casing. |
| 0-byte / empty file | walked, counted in `files_scanned`, contributes no matches. |
| Binary file with a scanned extension | read line-by-line like any file; the 1024-byte per-line cap skips long NUL-laden runs; short matching-extension binaries are rare in source trees and produce at most textual noise (not a crash). |
| File that fails to open (perms) | skipped silently; still counted toward `files_scanned`? No — an unopenable file is not counted. The walk continues. |
| Project root that doesn't exist / not a dir | the upstream root resolution returns empty → `no_project` (the lib is never reached). |
| A line matching both this language's def AND caller anchor | counted as a definition; excluded from callers (INV-9), all languages including Sh. |

## 4. Invariants

| # | Statement |
|---|-----------|
| 1 | `SymbolQuery` namespace in `src/symbolquery.{h,cpp}` exposes `parseLang`, `isValidSymbol`, `findDefinition(rootCanonical, symbol, opts)`, `findCaller(rootCanonical, symbol, opts)`. Qt6::Core only — no GUI deps; added to `ants_core_lib` in `CMakeLists.txt`. |
| 2 | `isValidSymbol` accepts exactly `^[A-Za-z_][A-Za-z0-9_]{0,127}$` and rejects everything else (empty, leading digit, regex metachars, `>128` chars). |
| 3 | The symbol is `QRegularExpression::escape`d before being spliced into any anchor, even though `isValidSymbol` already excludes metachars (defence in depth). |
| 4 | The tree walk skips dir components beginning with `.`, named `build` or beginning with `build-`, or named `node_modules`. Symlinks are not followed. |
| 5 | Only files whose lowercased extension is in the § 3.2 table are scanned; `Auto` scans all four families, an explicit `lang` only that family's. |
| 6 | Lines longer than 1024 bytes are skipped for regex matching but still advance the line counter, so the `line` reported for later matches in the same file stays correct (catastrophic-backtracking guard, mirrors `fileoutline.cpp` INV-8). No per-line-skip count is surfaced. CRLF endings are chopped before matching. |
| 7 | The walk stops scanning new files at `maxFiles` (default 5000) and sets `walkCapped:true`; partial results returned so far are still `ok:true`. |
| 8 | A C++ match is tagged `kind="namespace"` when it opens one (ANTS-4346), `kind="declaration"` when the line — comments stripped (ANTS-4369) — ends in `;` and opens no body, else `kind="definition"`. A brace opens a body when a parameter list, a lambda capture list or a class-key precedes it (ANTS-4358 / ANTS-4821), so `struct Foo { int a; };` is a definition and `QTimer *m_t{nullptr};` a declaration; a definition whose `{` opens on the *next* line is still `definition`. Py/Lua/Sh definition matches are always `kind="definition"`. |
| 9 | A line matching a language's *definition* anchor is excluded from that language's *caller* results (a definition is not its own call site). This holds for all four languages, including Sh — a one-line `S() { … }` def is not also reported as a caller. |
| 10 | `findCaller` attaches the best `DefMatch` (first definition, else first declaration, by file/line order) as `definition`; absent (`std::nullopt`) when none found. |
| 11 | `find_definition` caps `definitions[]` at `maxResults` (default 50, override via `max_results`); `find_caller` caps `callers[]` at default 200. `*_count` (`definitions_count` / `callers_count`) reflects the pre-cap total; `truncated:true` set when the cap dropped entries. |
| 12 | `cmdFindDefinition` / `cmdFindCaller` declared public in `remotecontrol.h` (return `QJsonDocument`); defined in `remotecontrol.cpp` with `ANTS-1303` anchor comments. |
| 13 | Both handlers validate `symbol` (→ `bad_args` on missing/empty/malformed) and resolve the root via `resolveRootCanonical(m_main, req)` (empty → `no_project`) before calling the lib. File paths in the envelope are project-relative (root prefix stripped). |
| 14 | Both verbs are reachable via the IPC `dispatch()` switch AND registered as MCP tools. |
| 15 | `MainWindow::setupClaudeMcpProviders` registers `find_definition` and `find_caller` via `registerToolProvider`, delegating to the `cmd*` methods, with the `kRcUnavailable` fallback when `m_remoteControl` is null. |
| 16 | `callerCwdContractFor` classifies `find_definition` and `find_caller` as `Required` (explicit branches — ANTS-1417 coverage). |
| 17 | `kindForName` classifies both as `"symbol"` — a new bucket branch added before the fall-through default (so neither falls into `"other"`; enforced by the ANTS-1567 prefix-tag coverage test at `tests/features/mcp_tool_prefix_tags/`). |
| 18 | The token-cost ledger in `claudeintegration.cpp` carries `{find_definition,{600,2500}}` and `{find_caller,{800,4000}}` (typical, cap). |
| 19 | The `claudeintegration.cpp` tools/list block registers both tools with `symbol` (required), `caller_cwd` (required), `lang` (optional), `max_results` (optional), `additionalProperties:false`, and a `selection_hint`. |
| 20 | (ANTS-3680) `findDefinitions(root, symbols, opts)` answers N needles in ONE walk and returns exactly what N `findDefinition` calls return — same anchors, ordering, kinds, caps and `fileStemHint`. `maxResults` caps each needle; `maxFiles` the shared walk. A duplicate needle is answered once; an invalid one gets its own `bad_args` result without failing the batch. It is a lib entry point only — no MCP verb. |
| 21 | (ANTS-3680) A line not containing the symbol's literal text runs no anchor: every anchor that consumes the symbol requires that literal on the line, and every anchor bounds it on both sides, so a needle inside a longer identifier matches nothing. That is what lets the batch gate on whole identifier tokens at a cost independent of the needle count. |

## 5. Memory & cost budget

- Per call: one `QDirIterator`, one `QFile` open at a time, one
  `QVector<*Match>` capped at `maxResults`, one `QJsonDocument`. No
  persistent state, no cache, no index — the walk is re-run each call.
- Worst-case heap: `maxResults` entries × (≤ 1 KiB path + signature) +
  one ≤ 1 KiB scanned line ≈ ≤ 250 KiB at default caps; the
  `max_results` arg is the load-bearing knob.
- Time: streamed single-pass per file, bounded by `maxFiles`. On this
  repo's ~350 source files a query is < ~50 ms.
- Deliberately NO index/cache: an index would need invalidation on
  every edit and a RAM budget of its own. ANTS-1637 (codebase index)
  is the place for a persistent structural map; this tool stays
  stateless. (If profiling later shows the per-call walk is a hot
  path, ANTS-1637's index becomes the backing store — not a new cache
  here.)

## 6. Out of scope

- Semantic resolution (overload sets, scoping, `using` aliases,
  macro-expanded names). v1 is textual-regex only; a symbol used in
  two unrelated classes returns both.
- Cross-language symbol linking (a Python `ctypes` call into a C++
  symbol is two unrelated textual matches).
- Member-access call detection for shell (no paren convention).
- Ranking callers by relevance. Order is (file, line); the caller
  filters.
- A persistent symbol index — see ANTS-1637.
- Following symlinks out of the project root.
- `find_references` (def + callers merged). The two-tool split mirrors
  the LSP `textDocument/definition` vs `textDocument/references`
  distinction and keeps each envelope focused; `find_caller` already
  bundles the definition for the common "where + who" case.

## 7. Doc-drift fold-in (apply in the same commit)

- Flip `ROADMAP.md` ANTS-1303 to ✅.
- Add a CHANGELOG entry to the unreleased section.
- `CLAUDE.md` `remotecontrol` module map verb list gains
  `find_definition`, `find_caller`.
- No new error codes for `docs/standards/mcp-error-codes.md`: the tool
  reuses `bad_args` (§ 1 input validation) for a malformed `symbol` and
  `no_project` (§ 2 resource state, line 52) for an unresolved root.
  Both already exist; the taxonomy's own guidance (§ "Adding a new
  code") is to reuse when one fits rather than proliferate.
