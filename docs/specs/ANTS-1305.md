# ANTS-1305 — `similar_code` MCP — pattern matcher for existing project shapes

**Status:** spec draft (2026-05-20).
**Kind:** implement.
**Source:** indie-review-2026-05-13 (roadmap bundle plan pull 39).

One new read-only MCP tool (plus a matching IPC verb) that answers
"show me the project's existing examples of this shape" before Claude
writes a new one. A new Qt6::Core-only `SimilarCode` lib walks the
source tree, extracts class/function signatures via the **existing**
`FileOutline` extractor, and ranks them by token-set similarity to a
free-text `shape` query. The MCP/IPC layer is a thin delegate.

## 1. Problem

CLAUDE.md §3 (reuse-before-rewriting) tells the assistant to copy the
project's existing dialog / IPC-verb / test pattern instead of
re-deriving one. In practice Claude either re-greps the tree from
scratch ("how do other dialogs subclass QDialog here?") or, worse,
invents a fresh convention that drifts from the codebase. The existing
tools half-cover this:

- `find_definition` (ANTS-1303) needs the *exact* symbol name — useless
  when you don't yet know what to call the thing you're about to write.
- `file_outline` (ANTS-1249) lists one file's symbols — but you don't
  know *which* file holds the closest prior art.
- `workspace_search` (ANTS-1248) greps a literal/regex — it can't rank
  by *shape* similarity, only by exact text.

The missing primitive: "given the shape `class FooDialog : public
QDialog` (which doesn't exist yet), what are the closest existing
shapes?" — a fuzzy, structural lookup.

## 2. Goal

One tool, `{shape, caller_cwd, lang?, max_results?}` in:

- **`similar_code`** → the N existing class/function signatures most
  similar to the `shape` query, structured as `matches:[{file, line,
  signature, kind, lang, score}]`, ranked by descending similarity.

Regex-anchored extraction (reusing `FileOutline`), no clangd /
tree-sitter / build dependency. C++ / Python aware (the families
`FileOutline` extracts code symbols for). The query is *tokenised*,
never spliced into a regex, so there is no injection surface and no
identifier-shape guard is needed.

## 3. Surface

### 3.1 New lib — `src/similarcode.{h,cpp}` (Qt6::Core only)

`namespace SimilarCode`. No GUI deps; lives in `ants_core_lib` next to
`fileoutline.cpp` (whose extractor it reuses) and `symbolquery.cpp`
(whose tree-walk shape it mirrors). Public API:

```cpp
enum class Lang { Auto, Cpp, Py };
Lang parseLang(const QString &s);   // "" / "auto" → Auto; "cpp"/"py" → that; else Auto

struct Match {
    QString file;       // project-relative (root prefix stripped)
    int     line = 0;
    QString signature;  // the matched signature line (trimmed by FileOutline)
    QString kind;       // FileOutline kind: "class" / "func" / "qt"
    QString lang;       // "cpp" / "py"
    double  score = 0;  // Jaccard similarity 0..1 (rounded to 4 dp)
};

// maxResults / maxFiles == 0 is the sentinel "use the default" (NOT
// "return nothing"). Defaults in § 3.4.
struct Options { int maxResults = 0; int maxFiles = 0; Lang lang = Lang::Auto; };

struct Result {
    bool ok = true; QString error, code;
    QVector<Match> matches;
    int  matchesTotal = 0;   // pre-cap count of score>0 candidates
    int  filesScanned = 0;
    bool truncated  = false;
    bool walkCapped = false;
};

// Tokenise a shape string into normalised tokens (exposed for tests).
QStringList tokenize(const QString &shape);
// Jaccard similarity |A∩B| / |A∪B| of two token sets (exposed for tests).
double jaccard(const QStringList &a, const QStringList &b);

Result findSimilar(const QString &rootCanonical, const QString &shape, const Options &opts);
```

The lib takes an already-canonical project root (the MCP/IPC caller
resolves + path-anchors it); the lib never escapes that subtree.

### 3.2 Tree walk

Mirrors `symbolquery.cpp`'s walk exactly (the two are independent
~30-line walks; per Rule of Three the helper is not yet extracted —
ANTS-1637's codebase index is the eventual shared backing store). From
`rootCanonical`, recurse with `QDir::entryInfoList`, **not** following
symlinks. Skip rules applied to each directory component:

- Any component beginning with `.` (`.git`, `.audit_cache`, `.claude`, …).
- A component named `build` or beginning with `build-`.
- A component named `node_modules`.

Included files, by lowercased extension → `FileOutline::Mode`:

| Lang | Extensions | Mode |
|------|-----------|------|
| Cpp  | `cpp cc cxx c h hpp hh hxx` | `Mode::Cpp` |
| Py   | `py pyi` | `Mode::Py` |

(The extension set matches `symbolquery.cpp`'s — wider than
`fileoutline.cpp`'s own `pickModeByExt`, which omits `c`/`hxx`/`pyi`.
That is exactly why the explicit `Mode` is passed: `FileOutline::compute`
honours a non-`Auto` mode and skips `pickModeByExt` entirely, so a `.c`
file scanned as `Mode::Cpp` gets the C++ regexes.)

In `Auto` both families are scanned; with an explicit `opts.lang`, only
that family's files. The explicit `Mode` is passed to
`FileOutline::compute` (not `Auto`), so `.c` / `.hxx` / `.pyi` are
handled as their family even though `FileOutline::pickModeByExt`
wouldn't recognise them on its own. The walk stops scanning new files
once `filesScanned` reaches `maxFiles` (default 5000) and sets
`walkCapped:true`.

### 3.3 Signature extraction (reuse `FileOutline`)

For each included file the lib calls
`FileOutline::compute(absPath, mode, /*includeDocComment=*/false,
/*maxSymbols=*/1000)` and iterates the returned `symbols[]` array. Each
symbol contributes one candidate `{line, kind, signature}`; `signature`
is FileOutline's already-trimmed matched line. Files whose compute
returns `ok:false` (unopenable) are skipped and **not** counted toward
`filesScanned`; a successful compute increments `filesScanned`.

This is the §3-reuse path: FileOutline already owns the
class/struct/namespace, member-def, free-func, and Qt-marker (`signals`
/ `slots` / `Q_PROPERTY` / …) regexes plus the 1024-byte per-line
backtracking cap (ANTS-1249 INV-8). `similar_code` adds the tree walk
and the ranking; it does **not** re-implement extraction.

### 3.4 Tokeniser, scoring, ranking, RAM

**Tokeniser** (`tokenize`): lowercase the string, split on every run of
characters outside `[A-Za-z0-9]`, drop tokens shorter than 2 chars.
Examples: `"class FooDialog : public QDialog"` →
`[class, foodialog, public, qdialog]`; `"void cmdBar(const QJsonObject&)"`
→ `[void, cmdbar, const, qjsonobject]`; `"cmd_bar"` → `[cmd, bar]`
(`_` is a separator). No camelCase splitting in v1 (`cmdBar` stays one
token) — documented in § 6.

**Scoring** (`jaccard`): convert both token lists to sets, return
`|Q ∩ C| / |Q ∪ C|` (0..1). Empty union → 0. The `shape` query is
tokenised once; each candidate signature is tokenised and scored
against it. Candidates with `score == 0` (no shared token) are dropped.

**Ranking & caps**: the lib keeps a bounded top-K (K = `maxResults`,
default 3 = `kDefaultResults`, hard cap 20 = `kMaxResultsCap` — both
named constants, novel to this tool since the use case wants a short
"top examples" list, not the 50/200 of the symbol queries) by
descending `score`, tie-broken by ascending `(file, line)` for
determinism. A caller-supplied `max_results > kMaxResultsCap` is
clamped to the cap. `matchesTotal` carries the pre-cap
count of all `score > 0` candidates; `truncated:true` when
`matchesTotal > matches.size()`. Boundary ties beyond the cap resolve
by walk order (`QDir::Name`), which is deterministic.

**RAM**: O(K) retained matches + one transient `QJsonObject` per file
(FileOutline's outline, discarded after scoring) + the query token set.
Peak working set is bounded by `K × (path + signature ≤ ~1 KiB)` plus
the single largest file's outline (≤ ~1000 symbols × ~1 KiB, the
FileOutline cap) — well under 2 MiB. No persistent cache, no index:
each call is a fresh streamed walk.

### 3.5 MCP / IPC wiring

- `RemoteControl::cmdSimilarCode(const QJsonObject&)` declared public in
  `remotecontrol.h` next to `cmdFindCaller`; defined in
  `remotecontrol.cpp` with `ANTS-1305` anchor comments. It: reads +
  validates `shape` (refuse `bad_args`), resolves the root via
  `resolveRootCanonical(m_main, req)` (refuse `no_project` when empty),
  reads optional `lang` / `max_results`, calls the lib, and serialises
  the struct to the § 3.6 envelope.
- Exposed as the IPC verb `similar-code` in `dispatch()` (sibling to
  `find-definition` / `find-caller`) AND as the `similar_code` MCP tool.
- `MainWindow::setupClaudeMcpProviders` registers `similar_code` via
  `registerToolProvider` with `CallerCwdContract::Required`, a
  pass-through lambda delegating to `cmdSimilarCode(args)`, with the
  `kRcUnavailable` fallback when `m_remoteControl` is null.

### 3.6 Envelope

Struct-field → wire-key mapping (the only non-1:1 renames): struct
`matchesTotal` → JSON `matches_count`; struct `walkCapped` → JSON
`walk_capped`; `filesScanned` → `files_scanned`. All others
lower-snake-case 1:1.

`similar_code` success:

```json
{ "ok": true, "shape": "class FooDialog : public QDialog", "lang": "auto",
  "matches": [
    {"file":"src/roadmapdialog.h","line":42,
     "signature":"class RoadmapDialog : public QDialog {",
     "kind":"class","lang":"cpp","score":0.6},
    {"file":"src/auditdialog.h","line":58,
     "signature":"class AuditDialog : public QDialog {",
     "kind":"class","lang":"cpp","score":0.6} ],
  "matches_count": 9, "files_scanned": 312,
  "truncated": true, "walk_capped": false }
```

Empty result is `ok:true` with an empty array (a shape with no
token-overlapping signature is a valid answer, not an error).

### 3.7 Refusal codes

| code | when |
|------|------|
| `bad_args` | `shape` missing, empty after trim, longer than 512 chars, or tokenises to zero usable tokens; or `max_results` present but not a positive integer. (Reuses the input-validation code per `mcp-error-codes.md` § 1.) |
| `no_project` | root unresolved (no `caller_cwd`, no focused tab). |

(`caller_cwd_required` is emitted upstream by the dispatcher per
ANTS-1404 before the handler runs, when `caller_cwd` is absent.)

### 3.8 Edge cases

| Input | Behaviour |
|-------|-----------|
| Shape with no token-overlapping signature anywhere | `ok:true`, empty `matches`, `matches_count:0`. |
| Shape matching > `maxResults` signatures | array capped, `truncated:true`, `matches_count` = full pre-cap total. |
| Shape that is all punctuation / 1-char tokens (`"a + b"`) | tokenises to zero usable tokens → `bad_args`. |
| `lang=py` against a C++-only shape | scans only `.py`/`.pyi`; returns whatever Python signatures share tokens (often empty) — not an error. |
| 0-byte / empty file | FileOutline returns ok with no symbols; counted in `files_scanned`, contributes no candidates. |
| Binary file with a scanned extension | FileOutline's 1024-byte per-line cap skips long NUL runs; at most textual noise, never a crash. |
| File that fails to open (perms) | FileOutline returns `ok:false`; skipped, **not** counted toward `files_scanned`. |
| Two signatures with identical score | both retained if within the cap; order is `(file, line)` ascending. |
| Project root that doesn't exist / not a dir | upstream resolution returns empty → `no_project` (the lib is never reached). |

## 4. Invariants

| # | Statement |
|---|-----------|
| 1 | `SimilarCode` namespace in `src/similarcode.{h,cpp}` exposes `parseLang`, `tokenize`, `jaccard`, `findSimilar(rootCanonical, shape, opts)`. Qt6::Core only — no GUI deps; added to `ants_core_lib` in `CMakeLists.txt`. |
| 2 | `findSimilar` reuses `FileOutline::compute` for per-file signature extraction; it does NOT re-implement the class/func/qt regexes. |
| 3 | The `shape` query is tokenised, never spliced into a regex — there is no injection surface and no identifier guard. |
| 4 | The tree walk skips dir components beginning with `.`, named `build` or beginning with `build-`, or named `node_modules`. Symlinks are not followed. |
| 5 | Only files whose lowercased extension is in the § 3.2 table are scanned; `Auto` scans Cpp+Py, an explicit `lang` only that family's. The explicit `FileOutline::Mode` is passed (so `.c`/`.hxx`/`.pyi` are covered). |
| 6 | `tokenize` lowercases, splits on runs of non-`[A-Za-z0-9]`, and drops tokens shorter than 2 characters. |
| 7 | `jaccard(a,b)` returns `|set(a) ∩ set(b)| / |set(a) ∪ set(b)|`, and 0 when the union is empty. |
| 8 | Candidates with `score == 0` are excluded from both `matches` and `matchesTotal`. |
| 9 | `matches` is ordered by descending `score`, tie-broken by ascending `(file, line)`. |
| 10 | `matches` is capped at `maxResults` (default 3, hard cap 20); `matches_count` carries the pre-cap count of `score>0` candidates; `truncated:true` when the cap dropped entries. |
| 11 | The walk stops scanning new files at `maxFiles` (default 5000) and sets `walkCapped:true`; partial results are still `ok:true`. |
| 12 | A `Match.file` is project-relative (root prefix stripped); never absolute, never containing the root path. |
| 13 | A successful `FileOutline::compute` increments `filesScanned`; an unopenable file (compute `ok:false`) does not. |
| 14 | `cmdSimilarCode` declared public in `remotecontrol.h` (returns `QJsonDocument`); defined in `remotecontrol.cpp` with `ANTS-1305` anchor comments. |
| 15 | `cmdSimilarCode` validates `shape` (→ `bad_args` on missing/empty/too-long/no-tokens) and resolves the root via `resolveRootCanonical(m_main, req)` (empty → `no_project`) before calling the lib. |
| 16 | The verb is reachable via the IPC `dispatch()` switch (`similar-code`) AND registered as an MCP tool (`similar_code`). |
| 17 | `MainWindow::setupClaudeMcpProviders` registers `similar_code` via `registerToolProvider` with a pass-through `cmdSimilarCode(args)` lambda and the `kRcUnavailable` fallback. |
| 18 | `callerCwdContractFor` classifies `similar_code` as `Required` (explicit branch — ANTS-1417 coverage). |
| 19 | `kindForName` classifies `similar_code` as `"pattern"` — a new bucket branch added before the fall-through default (so it does not fall into `"other"`; enforced by ANTS-1567 at `tests/features/mcp_tool_prefix_tags/`). |
| 20 | The token-cost ledger in `claudeintegration.cpp` carries `{similar_code,{600,2500}}` (typical, cap). |
| 21 | The `claudeintegration.cpp` tools/list block registers `similar_code` with `shape` (required), `caller_cwd` (required), `lang` (optional), `max_results` (optional), `additionalProperties:false`, and a `selection_hint`. |

## 5. Memory & cost budget

- Per call: one recursive walk, one `QJsonObject` outline per file
  (transient), one bounded top-K `QVector<Match>` (K ≤ 20), one query
  token set. No persistent state, no cache, no index.
- Worst-case heap: `K` matches × (≤ 1 KiB) + one file's outline
  (≤ 1000 symbols × ~1 KiB FileOutline cap) ≈ ≤ ~2 MiB transient; the
  outline is freed before the next file. `max_results` is the only
  result-side knob; `max_files` bounds the walk.
- Time: streamed single pass per file, bounded by `maxFiles`. On this
  repo's ~350 source files a query is well under ~100 ms (FileOutline
  per-file cost × file count).
- Deliberately NO index/cache: an index would need invalidation on
  every edit and a RAM budget of its own. ANTS-1637 (codebase index) is
  the place for a persistent structural map; this tool stays stateless.

## 6. Out of scope

- Languages other than C++ / Python. `FileOutline` has no code-symbol
  extractor for Lua / Shell, so they yield no candidates; richer
  language coverage rides on FileOutline gaining those extractors.
- Extraction fidelity is exactly `FileOutline`'s. Notably its Python
  `rxPy` anchors at column 0, so only **top-level** `def`/`class` are
  candidates — an indented method inside a class is not extracted. C++
  member definitions (`Class::method(...)`) and Qt markers ARE
  extracted. Improving Python method coverage is a FileOutline change,
  not a `similar_code` one.
- camelCase token splitting (`cmdBar` stays one token). A future
  enhancement could split intra-word case boundaries for finer matching.
- IDF / token weighting. v1 uses unweighted Jaccard, so ubiquitous
  tokens (`void`, `const`, `public`) count equally with distinctive
  ones; distinctive tokens still dominate the union, so ranking is
  usable. A two-pass IDF weighting is the obvious next step if results
  prove noisy.
- Structural / AST shape matching (e.g. "an MCP tool with an op-switch"
  as a grammar). v1 matches the *signature line* token set; phrase the
  query like the code shape you are about to write, not as English prose.
- A persistent symbol index — see ANTS-1637.
- Following symlinks out of the project root.

## 7. Doc-drift fold-in (apply in the same commit)

- Flip `ROADMAP.md` ANTS-1305 to ✅ (migrate the entry to the CHANGELOG
  section per the project convention).
- Add a CHANGELOG entry to the unreleased section.
- `CLAUDE.md` `remotecontrol` module-map verb list gains `similar-code`.
- No new error codes for `docs/standards/mcp-error-codes.md`: the tool
  reuses `bad_args` (§ 1 input validation) and `no_project` (§ 2
  resource state). Both already exist.
