# ANTS-5734 — Answer "where is it, who uses it, what documents it" in one call: `trace`

**Status:** accepted (2026-10-16).
**Kind:** feature.
**Source:** ROADMAP.md ANTS-5734 (claude-config-request-2026-10-16, user requirements relayed; peer asks from demoreel, pressless and oneup).
**Pairs with:** ANTS-5725 (the always-loaded set `trace` joins on ship).
**Composes with:** ANTS-1303 (`SymbolQuery`, whose walk rules this reuses), ANTS-5506 (`ants-mcpd --call`, the script form), ANTS-1252 (the session preamble's size cap), ANTS-4932 (the `ants-mcpd` process this runs in).

**Layman:** Claude can look up everything about a piece of code (where it is defined, what calls it, which tests and documents mention it) in one step instead of many searches, in any of the common programming languages.

## 1. Problem

After this ships, one `trace` call returns a symbol's definitions with start and end lines, its callers, its references sorted by kind, the tests that exercise it, and the document sections, spec invariants and roadmap items that cite it. It parses code with tree-sitter grammars built into `ants-mcpd`, so every Ants Terminal user gets the same languages whatever their distribution ships. It also accepts a non-code path and answers which tests, documents and config files name that file.

What is missing today:

1. **Sessions trace by hand, one grep at a time.** claude-config measured 200 transcripts on 2026-10-16: about 2,680 single-symbol grep calls against 23 calls to the tracing verbs, with about 1.45 billion tokens of reading and searching re-sent. That measurement is recorded on the roadmap item; it is not re-run here.
2. **The existing verbs are regex scanners.** `SymbolQuery::findDefinition` and `SymbolQuery::findCaller` (`src/symbolquery.h`) match line anchors with no parse tree. They report a definition's start line only (`DefMatch` has `file, line, signature, lang, kind`), so a review packet cannot cut a window from a definition without its own `grep -n` runs (demoreel and pressless, via claude-config).
3. **No verb joins code to documents.** `cited_by`, `doc_citations` and `spec_query` each answer one slice. A session that wants "what describes this function" calls three verbs and merges them itself.
4. **No verb answers "what reads this file"** for a non-code path such as a systemd unit (oneup, via claude-config).

## 2. Surface

### 2.1 The verb

`trace`, project-scoped, `CallerCwdContract::Required`, with its entry in `ClaudeIntegration::callerCwdContractFor`. Read-only. Its engine entry is `QJsonObject trace::serve(const QJsonObject &args, const QString &rootCanonical, const trace::Env &env = {})` in `ants_trace_lib`, not a `RemoteControl` method: `RemoteControl` is in `ants_mcpcore_lib`, which `ants-terminal` also links. `void mcp::registerMcpdOnlyVerbs(ToolSink &sink, const RegistryHost &host, trace::Env env = {})` registers the verb. Its handler resolves `caller_cwd` through `host.roots`, the root provider `src/mcpdmain.cpp` already hands `registerProjectScopedVerbs`, takes the nearest folder above it holding `.git` (`RunTraceVerb::projectRootFor`), and calls `serve` there. The handler is an `mcp::RcHandler` constructed directly, with `lane = DispatchLane::Bulk`, so a long parse runs on the bulk worker and does not hold up other verbs.

| Argument | Shape | Meaning |
|---|---|---|
| `caller_cwd` | string, required | Resolves the project root. |
| `symbol` | string | The name to trace. Either a bare name matching `^[A-Za-z_][A-Za-z0-9_]{0,127}$` (the `SymbolQuery` boundary), or up to four such segments joined by `::` or `.`. The last segment is the name. The others must equal the definition's innermost enclosing scopes, in order, so `Foo::bar` matches `ns::Foo::bar` and `ns::bar` does not. The scopes narrow `definitions` only; every other section matches the last segment. |
| `path` | string | A project-relative or absolute path to any file. Selects path mode (§ 2.7). |
| `lang` | string, optional | Restrict parsing to one language id from § 2.3. |
| `include` | array, optional | A subset of `definitions`, `callers`, `callees`, `references`, `tests`, `docs`, `spec_invariants`, `roadmap_items`. Default: all that the current ship supports. |
| `depth` | integer 1–3, optional | Caller and callee depth. Default 1. |
| `max_bytes` | integer, optional | The standard response cap, default 512 KiB. |
| `offset` | integer, optional | Row cursor from a previous reply's `next_offset`. |

Exactly one of `symbol` and `path` is required. These refuse the call with `bad_args`: neither or both given, a malformed `symbol`, an unknown `include` member, an unknown `lang`, or `depth` outside 1–3. A `path` that `PathValidation::validatePath` rejects refuses with `bad_path`. A `caller_cwd` that does not resolve to a directory refuses with `no_project`. In ship 1, `depth` above 1 and `include:["callees"]` refuse with `bad_args` naming ship 2, rather than silently answering at depth 1.

`ants-mcpd --call trace '<json>'` (ANTS-5506) is the script form. That path strips `fields` (ANTS-5506 INV-9), so `include` is the way a script asks for definitions only: `ants-mcpd --call trace '{"symbol":"rcRunRg","include":["definitions"]}'`.

### 2.2 The reply

```json
{
  "ok": true,
  "mode": "symbol",
  "symbol": "rcRunRg",
  "precision": "syntactic",
  "definitions": [
    {"file": "src/remotecontrol_workspace.cpp", "line": 81, "end_line": 173,
     "signature": "static QJsonObject rcRunRg(...)", "kind": "function",
     "scope": [], "is_declaration": false, "lang": "cpp"}
  ],
  "callers": [
    {"caller": {"name": "cmdWorkspaceSearch", "file": "src/remotecontrol_workspace.cpp", "line": 240},
     "file": "src/remotecontrol_workspace.cpp", "call_line": 301,
     "call_text": "const QJsonObject r = rcRunRg(args, root);", "depth": 1}
  ],
  "references": [
    {"kind": "string_or_config", "file": "tests/coverage-map.json", "line": 12, "text": "..."}
  ],
  "tests": {"files": ["tests/features/workspace_search/test_ws.cpp"],
            "ctest_patterns": ["WorkspaceSearch"], "selection": "map"},
  "docs": [
    {"file": "docs/specs/ANTS-1248.md", "heading": "4.2 The rg call", "heading_line": 40, "line": 52}
  ],
  "spec_invariants": [{"spec": "ANTS-1248", "inv": "INV-3"}],
  "roadmap_items": [{"id": "ANTS-1248", "headline": "..."}],
  "counts": {"definitions": {"total": 1, "returned": 1}},
  "not_covered": [{"reason": "no_grammar", "lang": "dart", "files": 2}],
  "scanned": {"files": 812, "files_parsed": 640, "files_with_parse_errors": 90,
              "languages": ["cpp", "python", "bash"], "walk_capped": false},
  "truncated": false,
  "next_offset": null
}
```

The values in that example are illustrative, not measured. Rules on the shape:

- **`definitions[]`** spans the whole definition node: `line` is its first line and `end_line` its last, both 1-based. `signature` is the node's text up to its body, folded to one line and cut at 200 bytes. `scope` lists the enclosing class, namespace or module names, outermost first. Every match is listed; an overload is a second entry. A declaration with no body (a C++ prototype) is listed with `is_declaration:true`.
- **`callers[]`** has one row per call site, not per caller. `caller` is the innermost definition enclosing the call, or `null` at file scope.
- **`references[]`** holds every other occurrence, each with one `kind`:

| `kind` | What it is |
|---|---|
| `import` | An include, import, use or require naming the symbol or its file. |
| `code` | An identifier in parsed code that is neither a definition, a call nor an import: a type use, a function pointer, a Qt `&Class::method`. |
| `comment` | A comment node in parsed code. |
| `string_or_config` | A string literal in parsed code, or a whole-word match in any other file that is neither Markdown nor in a § 2.3 language: JSON, YAML, TOML, INI, XML, HTML, Meson, `.desktop`, systemd units and the like (§ 9.2 says why these get no grammar). |
| `test` | Any of the above inside a test file (§ 2.6). |
| `unparsed` | A whole-word match in a file of a § 2.3 language whose grammar has not shipped yet, such as CMake before ship 3. |

  Markdown files are not `references`; they go to `docs[]` (§ 2.7).
- **`counts`** carries `{total, returned}` for every section, so a capped reply says how much it left out.
- **`not_covered[]`** names what the answer could not see: `no_grammar` (a language with files present but no grammar), `file_too_large`, `parse_timeout`, `ambiguous_basename` (§ 2.7), `source_unreadable` (§ 4), and `limits` (a fixed sentence per language from its query file, such as "calls through function pointers and Qt signal connections are not followed").
- **`scanned`** is the "a zero says what it looked at" block (`docs/standards/mcp-tools.md` step 5a).

**Ordering and paging.** Rows are emitted in a fixed order: section order as in the example, then file path, then line, then column. The byte cap cuts at a row boundary, never inside one, and sets `truncated:true` and `next_offset` to the index of the first row left out. A call repeating the same arguments with that `offset` returns the rows from there. Offset paging replaces spill offload for this verb, so `trace` is exempt from `mcp::offloadThresholdFor`'s threshold. The reply is never trimmed silently.

### 2.3 The engine

- **Runtime.** tree-sitter 0.27.1, vendored under `third_party/tree-sitter/` (its `lib/src` and `lib/include`, with its MIT `LICENSE`).
- **Grammars.** Each one is vendored under `third_party/tree-sitter-grammars/<lang>/`: the generated `src/parser.c`, `src/scanner.c` where upstream has one, any other `src/*.h` beside it, `src/tree_sitter/*.h`, and the grammar's `LICENSE`. Every grammar is pinned in `third_party/tree-sitter-grammars/MANIFEST.json`:

```json
{"lang": "cpp", "repo": "https://github.com/tree-sitter/tree-sitter-cpp",
 "commit": "<sha>", "sha256": "<of the fetched tarball>", "license": "MIT",
 "generated_by": "upstream"}
```

  The manifest's `runtime` entry pins tree-sitter itself the same way. A grammar entry may also carry `subdir` (where one repository holds two grammars, as tree-sitter-typescript holds `typescript` and `tsx`), `extra` (shared files copied beside the grammars, such as that repository's `common/scanner.h`) and `license_file` (where the licence is not named `LICENSE`).

  A grammar is pinned to its latest release (its latest tag where it publishes no release), or to a later commit where that commit fixes a defect the release still has. tree-sitter-ruby is pinned to `ad907a69`, which fixes heredoc serialization buffer overflows in its scanner (committed 2026-03-10; its latest release, v0.23.1, is from 2024-11).

  `generated_by` is `upstream` when the repository commits `parser.c`. Otherwise it names the tree-sitter CLI version that generated the file (tree-sitter-swift does not commit its `parser.c`). The runtime accepts grammar ABI 13 to 15 (`TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION` and `TREE_SITTER_LANGUAGE_VERSION` in `api.h`). A refresh script, `tools/vendor-grammars.sh`, fetches each pinned commit, checks its hash, and copies the files. Nothing in the build fetches anything.
- **Build.** A static C library, `ants_treesitter_lib`, holds the runtime and the grammars. A Qt6::Core library, `ants_trace_lib`, holds the engine (`src/trace/`). Both link into `ants-mcpd` and the trace test bundle only, never into `ants-terminal`.
- **Languages.** A language is identified by file name, then by file extension, then by a shebang line for files with no extension. The names are `CMakeLists.txt` (`cmake`) and `Makefile`, `makefile` and `GNUmakefile` (`make`); `.mk` and `.mak` are `make` too. `.c` is `c`. Every other extension `SymbolQuery`'s `langForExt` maps to `Lang::Cpp`, `.h` included, is `cpp`.

| Ship | Languages (ids) |
|---|---|
| 1 | `c`, `cpp`, `python`, `bash`, `lua`, `glsl`, `javascript`, `typescript`, `tsx`, `rust`, `go`, `java` |
| 3 | `c_sharp`, `kotlin`, `swift`, `php`, `ruby`, `cmake`, `scala`, `dart`, `zig`, `elixir`, `haskell`, `ocaml`, `objc`, `r`, `julia`, `nix`, `qmljs`, `sql`, `perl`, `make`, `css` |

  Ship 3 lands in batches. The first adds `c_sharp`, `ruby`, `qmljs`, `make` and `css` (the user's request through Groundwork, 2026-10-16). The second adds `scala`, `zig`, `julia` and `objc` (2026-10-20); `elixir` waits on query predicates (ANTS-5792). The third adds `kotlin`, `cmake`, `haskell`, `r` and `nix` (2026-10-20). `ocaml` waits with `php`: its `scanner.c` includes `../../../common/scanner.h`, the same vendoring question. `sql` and `perl` commit no `parser.c` at their latest release, only on a generated branch, and `dart` publishes no release or tag. `php` follows once § 12's first open question is settled (ANTS-5751); it is the grammar for PHP mixed with HTML, from tree-sitter-php's `php` subdirectory. `qmljs` comes from `yuja/tree-sitter-qmljs`, a personal repository: no tree-sitter organisation hosts a QML grammar (`gh api repos/tree-sitter-grammars/tree-sitter-qmljs` → 404, 2026-10-16).

  `tools/vendor-grammars.sh` does not yet copy the other `src/*.h` files; this batch adds that, for qmljs's `typescript-scanner.h`. tree-sitter-php's `php/src/scanner.c` includes `../../common/scanner.h`, the path tree-sitter-typescript's `extra` already fills (`third_party/tree-sitter-grammars/common/scanner.h`; read from both repositories, 2026-10-16). The two files must be vendored to different paths, and how is open (§ 12).

  Ship 1 covers every language `SymbolQuery::Lang` names except its `Generic` brace family, plus the most used modern languages. A ship-3 grammar is added only after its upstream repository has been checked as maintained, MIT- or Apache-licensed, and buildable from vendored sources. One that fails stays `no_grammar` and gets a roadmap note. A language with files present but no grammar is reported, never dropped (INV-9).

### 2.4 Queries are data

Each language has one query file, `trace-queries/<lang>.scm`, using these captures:

| Capture | Marks |
|---|---|
| `@definition.<kind>` with a nested `@name` | A definition. `<kind>` is `function`, `method`, `class`, `type`, `module`, `macro`, `constant` or `field`. |
| `@declaration.<kind>` with a nested `@name` | A declaration with no body. |
| `@reference.call` with a nested `@name` | A call site. |
| `@reference.import` with a nested `@name` or `@path` | An import. |
| `@scope` | A node whose `@name` child is a scope name for `definitions[].scope`. |

A leading comment line `;; limits: <sentence>` supplies the language's `not_covered` `limits` entry. Where upstream ships a `tags.scm`, the file starts from it. Upstream's are not used as shipped: the C++ one has no call pattern, and its `function_declarator` pattern also matches prototypes (read from `/usr/share/tree-sitter/queries/cpp/tags.scm`, 2026-10-16).

The source copies live in `assets/trace-queries/` and are compiled into `ants-mcpd` as Qt resources, so an uninstalled build and the test bundle have them. At call time a file in `${XDG_DATA_HOME:-~/.local/share}/ants-terminal/trace-queries/` replaces the built-in copy for that language where it exists. A file in that directory is re-read when its mtime or size changes. A query file that fails to compile makes that language `no_grammar`, with `detail` naming the query error; the call does not fail.

### 2.5 The walk, the parse and the cache

- **The walk** reuses `SymbolQuery`'s rules: it skips dot-directories, `build`, `build-*` and `node_modules`, never follows a symlink, and stops after visiting 5000 regular files of any kind (`walk_capped:true`). Those rules (`skippedDir` and the symlink test in `walk`, `src/symbolquery.cpp`) move to a shared header so the two walks cannot drift.
- **The parse.** A file over 2 MiB is not parsed (`file_too_large`). A parse is abandoned after 2 seconds through `TSParseOptions::progress_callback` (`parse_timeout`). A tree with error nodes is still queried, and the file is counted in `files_with_parse_errors`. Measured 2026-10-16 with the CLI and the system C++ grammar: 90 of the 396 files in `src/` parse with errors, and files with errors yield definitions at the same rate as clean ones. In the 72 `.cpp` files with errors, a `function_definition` query found 604 definitions against 618 column-0 closing braces; in 60 clean ones, 1054 against 1100. Recipe: `tree-sitter parse --stat`, then `tree-sitter query` with that pattern.
- **The cache** keeps, per file, the extracted rows (definitions, calls, imports, identifier hits), never the tree. It is keyed by path, mtime, size and the query file's hash. Budget and eviction are § 5's.
- **Name matching is syntactic.** A caller is any call whose `@name` text equals the symbol's last segment. No type resolution is done, so two unrelated functions with one name share callers. That is why the reply says `precision:"syntactic"`, and why `scope` lets a caller narrow the definitions.

### 2.6 Tests

A test file is one under the project's `test_roots` (`.ants/project.json`, else the `project_layout` default). `tests.files[]` lists each test file holding a `callers` row or a `references` row for the symbol. `tests.ctest_patterns` is `FocusedTest::resolve` run over the files holding the symbol's definitions, with `tests.selection` set to `map` or `heuristic`. Where `resolve` falls back to the full suite, both are absent: "run everything" says nothing about this symbol.

### 2.7 Documents, specs, roadmap and path mode

- **`docs[]`**: every Markdown file the walk (§ 2.5) reaches. A whole-word match outside a fenced block, or any match inside a code span, gives one row per line. The row carries the nearest heading above it and that heading's line.
- **`spec_invariants[]`**: each invariant whose body, as `specparse` returns it, contains the symbol as a whole word.
- **`roadmap_items[]`**: each bullet, read the way `roadmap_query` reads it (the store where the project has been migrated into it, else its roadmap file), whose headline or body contains the symbol as a whole word, with its id and headline.

**Path mode** (`path` given) skips definitions, callers and callees. It searches for the file's project-relative path, and for its basename where the basename is unique in the project. It returns `references`, `tests`, `docs`, `spec_invariants` and `roadmap_items` under the same rules, except that `FocusedTest::resolve` runs over the path itself. A basename that is not unique is reported in `not_covered` as `ambiguous_basename` and not searched.

### 2.8 Registration

- `trace` registers through a new `mcp::registerMcpdOnlyVerbs`, in `ants_trace_lib`. `src/mcpdmain.cpp` calls it after `mcp::registerProjectScopedVerbs` and before the `--call` early return, so `ants-mcpd --call trace` reaches it (INV-21). Its `tools/list` descriptor is emitted only by a process that registered it. The Lua-only `project_query` descriptor, guarded by `ANTS_LUA_PLUGINS`, is the compile-time precedent. This one is decided at run time, because one library serves both binaries.
- **On ship 1**, `trace` joins `kEagerVerbs` (`src/claudeintegration.cpp`), and a line in `hooks/ants-session-preamble.sh` names it within that hook's 500-byte stdout cap (ANTS-1252 INV-3). claude-config is then told, so it can do CFG-0816 and CFG-0818.

### 2.9 Ships

| Ship | Adds |
|---|---|
| 1 | The verb; `definitions`, depth-1 `callers`, `references`, `tests`, `docs`, `spec_invariants`, `roadmap_items`, path mode; ship-1 languages. |
| 2 | `depth` 2–3 and `callees`. A depth-N walk resolves each caller's name to its definitions and repeats. Every call site is a row, but each definition is expanded once, the traced definitions counting as already expanded. The walk stops at 200 caller rows, while `counts.callers.total` keeps counting. Callees are the `@reference.call` names inside a definition's byte range, each with its resolved definitions. |
| 3 | The ship-3 grammars, each with its query file. |

### 2.10 Settled while building

Ship 1's build settled these. Each is what the code in `src/trace/` does.

- **Only files that hold the name are parsed.** A file whose text has no whole-word match is read and skipped, so `scanned.files_parsed` counts the files parsed for this name, not every file in a vendored language.
- **The roadmap read is the host's.** `serve` takes a `trace::Env`. `ants-mcpd` binds its `roadmapBullets` to `RemoteControl::roadmapBulletsForTrace`, which reads the store where the project is migrated, else the roadmap file. Without a binding, `serve` parses the roadmap file named by `.ants/project.json`'s `roadmap`, else `ROADMAP.md`.
- **`lang` filters parsing only.** Files in other § 2.3 languages are skipped; Markdown and files in no § 2.3 language are still searched.
- **Test roots** follow `codebaseindex.cpp`: the declared `test_roots` that exist, else `tests`.
- **A spec's id** is its file stem's leading `PREFIX-NNNN`, else the whole stem.
- **The 2 MiB limit applies to every file**, Markdown and config included, each reported `file_too_large`.
- **One offset spans every section.** `tests.files` rows and every other section's rows share one row sequence, in § 2.2's section order.
- **The descriptor** carries a short `description` and a `detail` that `tool_info` serves, since an always-loaded verb's wire description is capped (`tests/features/mcp_eager_load/`). Its kind prefix is `[symbol]`, as `find_definition`'s.
- **Licences** install to `share/ants-terminal/licenses/` as `LICENSE.tree-sitter` and `LICENSE.tree-sitter-<lang>`.
- **The fixture's roadmap** is `docs/roadmap.md`, named in its `.ants/project.json`, because a hook refuses a new file called `ROADMAP.md`.

## 3. Invariants

Fixtures live in `tests/features/trace/fixture/`, a small project with one file per ship-1 language, a test directory, docs, a spec and a roadmap file. Each case copies it into a temporary directory outside any repository and passes that directory as `caller_cwd` (`rootCanonical` for a direct `serve` call). Inside this repository, this spec's own text would match the fixture names.

- **INV-1** — Exactly one of `symbol` and `path` is accepted, and a `symbol` outside the § 2.1 shape refuses `bad_args`. *Test:* `tests/features/trace/` calls with neither, with both, and with `symbol:"a b"`, expecting `bad_args` each time. *Breaks when:* a missing selector defaults to an empty-name scan, or a space reaches the walk.
- **INV-2** — A definition's `line` and `end_line` are the first and last lines of the whole definition node. *Test:* `tests/features/trace/` traces `spanFn` in `fixture/span.cpp`, a function that opens on line 3 and closes on line 9, expecting `line:3, end_line:9`. *Breaks when:* the range is taken from the `@name` node, which gives `end_line:3`.
- **INV-3** — A C++ prototype and its definition are both listed, told apart by `is_declaration`, and two overloads give two entries. *Test:* `tests/features/trace/` traces `overloaded` in `fixture/overloads.{h,cpp}`, expecting one declaration entry and two definition entries. *Breaks when:* entries are de-duplicated by name, or the prototype pattern is captured as `@definition`.
- **INV-4** — Each call site is one `callers` row, whose `caller` is the innermost enclosing definition. *Test:* `tests/features/trace/` traces `callee` in `fixture/calls.py`, where `outer` calls it twice and a nested `inner` once, expecting three rows: two naming `outer`, one naming `inner`. *Breaks when:* rows are merged per caller, or the outermost definition is chosen.
- **INV-5** — Each occurrence gets exactly one `references` kind from § 2.2. *Test:* `tests/features/trace/` traces `kindProbe`, which the fixture names once in each of: an `#include`, a type use, a comment, a string literal, `fixture/config.json`, a file under `fixture/tests/`, and `fixture/script.dart`. It expects one row each of `import`, `code`, `comment`, `test` and `unparsed`, and two of `string_or_config`. *Breaks when:* a test file's hit is classed by its node type instead of `test`, or a config hit is dropped because the file is not parsed.
- **INV-6** — A document match carries its nearest heading and that heading's line. *Test:* `tests/features/trace/` traces `docProbe`, which `fixture/docs/guide.md` names once under `## Setup` and once inside a fenced block, expecting exactly one `docs` row, with `heading:"Setup"` and its line. *Breaks when:* the heading is taken from the file's first heading, or a match inside a fenced block is counted.
- **INV-7** — A spec invariant and a roadmap bullet naming the symbol are returned by id. *Test:* `tests/features/trace/` traces `specProbe`, named in INV-2 of `fixture/docs/specs/FIX-0001-probe.md` and in bullet `FIX-0002` of the fixture roadmap, expecting `{spec:"FIX-0001", inv:"INV-2"}` and `{id:"FIX-0002"}`. *Breaks when:* only the spec's header is searched, or the roadmap read is skipped.
- **INV-8** — A capped reply is never trimmed silently, and paging rebuilds the whole answer. *Test:* `tests/features/trace/` traces `pagedProbe`, named on 300 fixture lines, with a `max_bytes` that fits about 50 rows, and follows `next_offset` to the end. It expects `truncated:true` on every page but the last, no row cut mid-way, and the pages joined equal to one uncapped call in the same order. *Breaks when:* the order is not total, so two rows on one line swap between calls, or the cap drops rows without setting `next_offset`.
- **INV-9** — A source file in a language with no grammar is never dropped. *Test:* `tests/features/trace/` traces `kindProbe` and expects `not_covered` to hold `{reason:"no_grammar", lang:"dart", files:1}`, alongside INV-5's `unparsed` row. *Breaks when:* files of an unknown language are skipped by the walk.
- **INV-10** — A trace that finds nothing says what it looked at. *Test:* `tests/features/trace/` traces `noSuchSymbol`, expecting `ok:true`; `definitions`, `callers`, `references`, `tests.files`, `docs`, `spec_invariants` and `roadmap_items` empty; `scanned.files` above zero and `scanned.languages` listing the fixture's languages. *Breaks when:* an empty answer omits `scanned`, or refuses `not_found`.
- **INV-11** — Path mode finds the tests, documents and config that name a file, and refuses a path outside the root. *Test:* `tests/features/trace/` calls with `path:"units/probe.service"`, named in `fixture/tests/test_units.py` and `fixture/docs/guide.md`, expecting one `tests.files` entry and one `docs` row. A call with `path:"../../etc/passwd"` expects `bad_path`. In a temporary copy of the fixture with a second `probe.service` under another directory, the first call expects a `not_covered` entry `{reason:"ambiguous_basename"}` and no row matched by the basename alone. *Breaks when:* a non-unique basename is searched, or path validation is skipped.
- **INV-12** — The walk never follows a symlink. *Test:* `tests/features/trace/` adds a symlink to a temporary copy of the fixture, pointing at a directory outside it whose file names `kindProbe`, and expects no row from that directory. *Breaks when:* the walk follows a symlink into a directory.
- **INV-13** — `ants-mcpd` lists and serves `trace`; the terminal's in-process server lists none. *Test:* `tests/features/trace/` builds the `tools/list` reply through `ClaudeIntegration` once with `mcp::registerMcpdOnlyVerbs` run and once without, expecting `trace` in the first and absent from the second. *Breaks when:* the descriptor is emitted unconditionally, which advertises a verb the terminal cannot run.
- **INV-14** — An edited query file changes the next answer without a restart. *Test:* `tests/features/trace/` points `XDG_DATA_HOME` at a temporary directory, traces `queryProbe` once, then writes a `python.scm` there that drops `@reference.call`, and traces again in the same process. It expects callers on the first call and none on the second. *Breaks when:* query files are loaded once at start-up, or the cache key leaves out the query hash.
- **INV-15** — A file whose parse has errors still yields its definitions, and is counted. *Test:* `tests/features/trace/` traces `afterError` in `fixture/broken.cpp`, which defines it after a `signals:` block, expecting one definition and `files_with_parse_errors` of at least 1. *Breaks when:* a tree with error nodes is discarded.
- **INV-16** — A file over 2 MiB is reported, not parsed. *Test:* `tests/features/trace/` writes a 3 MiB `.py` file naming `kindProbe` into a temporary copy of the fixture, expecting a `not_covered` entry `{reason:"file_too_large"}` naming it and no row from it. *Breaks when:* the size check runs after the parse.
- **INV-17** — `ants-mcpd` carries the vendored runtime and links no shared tree-sitter library. *Test:* `nm build/ants-mcpd | grep -c ' [Tt] ts_parser_new$'` → `1` (the symbol is defined in the binary's text; global or local depends on the linker), and `ldd build/ants-mcpd | grep -c tree-sitter` → `0`. A binary linked with `-ltree-sitter` gives `0` for the first and `1` for the second. *Breaks when:* the build links the system `libtree-sitter` in place of the vendored runtime. A `dlopen` of a system grammar is outside what this test sees.
- **INV-18** — In ship 1, `depth` above 1 and `include:["callees"]` refuse `bad_args`. Ship 2 withdraws this and adds INV-19 and INV-20. *Test:* `tests/features/trace/` calls once with `depth:2` and once with `include:["callees"]`, expecting `bad_args` both times. *Breaks when:* the deeper request is accepted and answered at depth 1.
- **INV-19** — (Ship 2.) A depth-2 trace returns callers of callers with `depth:2`, and terminates on recursion. *Test:* `tests/features/trace/` traces `leaf` with `depth:3` in `fixture/chain.py` (`top` calls `mid`, `mid` calls `leaf`, `leaf` calls `mid`), expecting `mid` at depth 1, `top` and `leaf` at depth 2, and no depth-3 row: `leaf` is the traced definition, so it is not expanded again. *Breaks when:* a visited definition is walked again.
- **INV-20** — (Ship 2.) Callees are the calls inside a definition's own range. *Test:* `tests/features/trace/` traces `mid` with `include:["callees"]`, expecting `leaf` and not `top`. *Breaks when:* the range is the whole file.
- **INV-21** — The script form reaches `trace`. *Test:* `tests/features/trace/` runs `ants-mcpd --call trace '{"symbol":"spanFn","include":["definitions"]}'` from the fixture's temporary copy, expecting exit `0` and one definition with `end_line:9`. *Breaks when:* `mcp::registerMcpdOnlyVerbs` runs after the `--call` early return in `src/mcpdmain.cpp`.

## 4. Failure modes

- **A grammar crashes on a file.** Grammars are generated C and can crash. A crash takes down `ants-mcpd`, not the terminal: clients see the server drop and reconnect. Each grammar's upstream commit is pinned, so a crash is reproducible. A file that crashes the parser is not yet skipped automatically (§ 6).
- **A query file has a syntax error.** That language reports `no_grammar` with the error (§ 2.4); other languages still answer.
- **The roadmap or a spec is unreadable.** That section is empty, and `not_covered` names `{reason:"source_unreadable", source:"roadmap"}`. The call still succeeds.
- **A common name.** A name like `init` matches thousands of rows. The byte cap and paging bound the reply (INV-8); the 200-row caller stop bounds a depth-N walk (ship 2).
- **Two functions share a name.** Their callers merge (§ 2.5). `scope` narrows definitions but not callers; `precision:"syntactic"` says so.
- **A file changes during a call.** The rows come from the parse that ran; the next call sees the new mtime and re-parses.
- **A grammar's ABI is outside 13–15.** `ts_parser_set_language` fails at first use. That language becomes `no_grammar` with the reason. A load test in the bundle (§ 7) sets every vendored language once, so this is caught before release.

## 5. RAM / build cost

- **Disk.** The installed system grammar libraries for the ship-1 languages total 12.0 MB; those for `c_sharp`, `kotlin`, `swift`, `php` and `ruby` total 15.5 MB (`stat -c %s /usr/lib64/tree-sitter/libtree-sitter-<lang>.so`, summed, 2026-10-16). Vendored grammars compiled into `ants-mcpd` are expected near those figures. `ants-mcpd` is 13.0 MB today (`build/ants-mcpd`, Release, 2026-10-16). Grammar data is paged in only for the languages a call parses.
- **Build.** The C++ grammar's generated `parser.c` is 17 MB. It compiled in 1.24 s at `-O2` with a 169 MB peak (`/usr/bin/time cc -O2 -c parser.c`, 2026-10-16). C#'s is 29.7 MB (`gh api repos/tree-sitter/tree-sitter-c-sharp/contents/src/parser.c?ref=v0.23.5`, `size`, 2026-10-16).
- **Memory.** The row cache (§ 2.5) is capped at 64 MiB and evicts the least recently used file first. It lives in memory and goes when `ants-mcpd` exits. A tree is freed once its rows are extracted. A cold `trace` parses every file once: the CLI parsed all of `src/` (396 files, 11 MB) in 1.44 s with a 22 MB peak (`/usr/bin/time tree-sitter parse --quiet --stat … src/*.cpp src/*.h`, 2026-10-16).
- **New dependencies.** tree-sitter (MIT) and one grammar per language, each licence recorded in `MANIFEST.json`, all vendored. No new build-time or run-time package.

## 6. Out of scope

- **An LSP precision layer** — deferred; not yet queued. § 9.2 says why.
- **Skipping a file that crashed the parser on an earlier run** — deferred; not yet queued.
- **Type-aware call resolution** (virtual dispatch, function pointers, Qt signal and slot connections) — reported in `not_covered.limits`; deferred with the LSP layer, not yet queued.
- **Shrinking the always-loaded schemas** — tracked by ANTS-5725.
- **Retiring `find_definition` and `find_caller` in favour of `trace`** — deferred; not yet queued.
- **A persistent on-disk index** — excluded, not deferred: a cold parse is fast enough (§ 5), and an index brings invalidation bugs.

## 7. Tests

Feature test: `tests/features/trace/`, label `features`, its bundle taken from `build_target_for` once it exists. Every case runs against `tests/features/trace/fixture/`. Each case is run against the code before the change that makes it pass, and is seen to fail there.

- INV-1, INV-2, INV-3, INV-4, INV-5, INV-6, INV-7, INV-8, INV-9, INV-10, INV-11, INV-12, INV-15, INV-16 and INV-18: one case each, calling `trace::serve` directly.
- INV-13: builds two `ClaudeIntegration` pipelines and compares their `tools/list` replies.
- INV-14: the query re-read case, with `XDG_DATA_HOME` set for the test process.
- INV-17: the `nm` and `ldd` commands, run by the bundle against the built `ants-mcpd`.
- INV-21: runs the built `ants-mcpd` with `--call`.
- INV-19 and INV-20: added with ship 2.
- A load test sets each vendored language on a parser once. It locks the ABI failure mode in § 4, not an invariant.

The fixture's `.dart` file is never parsed; it exists to be reported.

## 8. Reload

- **Own process.** `trace` runs in `ants-mcpd` only (§ 2.8), so its grammars stay out of the terminal binary and a grammar fix never needs a terminal relaunch, which would end every Claude Code session in its tabs.
- **Reaching a running terminal.** A new `ants-mcpd` build reaches clients on their next reconnect (`/mcp`), with no terminal relaunch. A query file placed in the user directory is re-read on change (§ 2.4), so a query fix can be tried with no rebuild.
- **Stop/start control.** The only running state is the row cache. It goes when `ants-mcpd` restarts, so a reconnect is the stop/start control and no separate one is needed.

## 9. Decisions and alternatives

### 9.1 Decisions

- **Documents count as uses, and every modern language is covered for every user, not only this machine.** The user's two requirements, relayed by claude-config 2026-10-16.
- **Grammars are compiled into `ants-mcpd`, not loaded from system packages.** The user's lean, relayed 2026-10-16; this spec confirms it. Distribution packages differ, Flatpak cannot see the host's `/usr/lib64`, and Tumbleweed ships no CMake grammar. A vendored grammar behaves the same for every user.
- **Ship order** (§ 2.9) is the roadmap item's.
- **No LSP in this spec**, and **`trace` hosted by `ants-mcpd` alone.** This session's decisions, 2026-10-16, put to the user in the handoff.

### 9.2 Rejected

- **System grammar packages, loaded with `dlopen`.** Packages differ by distribution, and Flatpak cannot see the host's libraries. Tumbleweed has no CMake grammar, and it ships `grammar.js` without `scanner.c`, so its packages cannot be rebuilt from what they install.
- **WebAssembly grammars through tree-sitter's wasm runtime.** It adds a WebAssembly engine as a dependency to solve a portability problem vendored C sources already solve.
- **LSP servers where installed** (claude-config's recommendation). Deferred, not rejected. Each language needs its own server process, lifecycle and project setup (clangd needs `compile_commands.json`), and first answers wait on indexing. Answers would also differ between machines with and without a server, which works against "every user". A later item can add LSP as a layer that refines `precision:"syntactic"` rows where a server answers.
- **Extending `SymbolQuery`'s regex scanner.** It has no node ranges (so no `end_line`), no scopes, and no comment or string boundaries: the three things this verb adds.
- **Grammars for data and markup formats** (JSON, YAML, TOML, XML, HTML, Markdown). Their keys and elements would become `definitions`, so tracing a common code name such as `id` or `source` would list every config key of that name beside its real definitions. A whole-word match in those files already arrives as a `string_or_config` reference, and Markdown goes to `docs[]`. Decided by this session 2026-10-16, on the user's leave to choose.
- **A Meson grammar.** The one the tree-sitter-grammars organisation hosts, `tree-sitter-meson`, says in its README that a comment can break the parse (v1.3.0). Meson files stay `string_or_config`.
- **Compiling the grammars into both binaries.** It adds about 12 MB to `ants-terminal` in ship 1, and a grammar fix would then need a terminal relaunch.

## 10. What checks this

| Rule | What catches a breach |
|------|----------------------|
| INV-1 to INV-16, INV-18 | `tests/features/trace/` |
| INV-17 | **Partial:** `tests/features/trace/` (the `nm` and `ldd` checks) catches a shared link; nothing catches a `dlopen` of a system grammar |
| INV-21 | `tests/features/trace/` |
| INV-19, INV-20 | `tests/features/trace/`, from ship 2 |
| § 2.3 grammar pins match the vendored files | **nothing** — `tools/vendor-grammars.sh` checks hashes only when it runs; deferred; not yet queued |
| § 2.8 preamble stays within 500 bytes | ANTS-1252 INV-3's test |

## 11. Cross-doc impact

- `docs/subsystems.md`: a new trace subsystem (`src/trace/`, `third_party/`).
- `.indie-review/partition.json`: `src/trace/` joins the `code-index-search` lane, beside `SymbolQuery`.
- `docs/standards/mcp-behavioural-notes.md`: a `trace` entry.
- `CHANGELOG.md`: one entry per ship.
- Packaging (`packaging/*`): the vendored licences, where a package lists third-party licences.
- `hooks/ants-session-preamble.sh` and `kEagerVerbs`: on ship 1 (§ 2.8).

## 12. Open questions

- **LSP.** This spec defers it (§ 9.2). The user may want it in ship 2 instead.
- **Where tree-sitter-php's `common/scanner.h` is vendored** (§ 2.3). The layout, the `MANIFEST.json` field that places it, `tools/vendor-grammars.sh` and `CMakeLists.txt` must agree, and `php` waits on the answer.
- **The CMake grammar.** `uyha/tree-sitter-cmake` is claude-config's suggestion. Whether it is maintained and licensed for vendoring is unchecked; ship 3's admission rule (§ 2.3) decides it.

## Cold-eyes loop log

Rows live in [`../reviews/ANTS-5734-trace-verb-loop-log.md`](../reviews/ANTS-5734-trace-verb-loop-log.md).
