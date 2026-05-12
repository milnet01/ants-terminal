# ANTS-1249 — `file_outline` MCP tool

**Status:** spec, ship-ready after cold-eyes pass.
**Source:** user-2026-05-12 (token-reduction priority).
**Siblings:** ANTS-1247, 1248, 1250, 1251, 1252.

## 1. Problem

Claude reads whole files to learn what they contain. Measured against
this repo (2026-05-12):

| File | Lines | Bytes | ~Tokens | Func-like decls |
|------|------:|------:|--------:|-----------------:|
| `src/claudeintegration.cpp` | 1 599 |  70 116 | ~17 K | 37 |
| `src/terminalgrid.cpp`      | 3 157 | 138 781 | ~35 K | 56 |
| `src/auditdialog.cpp`       | 5 659 | 267 728 | ~67 K | 63 |
| `src/roadmapdialog.cpp`     | 2 701 | 122 120 | ~31 K | 24 |

When the assistant only needs to know *what's in* a file (to pick
where to grep, to decide which lane a task belongs in, to answer
"where is X defined"), reading the whole thing wastes ~95-99% of the
tokens. An outline (top-level comment block + per-symbol
`<file>:<line> <signature>`) carries the same orientation signal at
~1-3 K tokens for a 67 K-token file — **~20-65× compression**.

## 2. Surface

| Field | Type | Default | Notes |
|-------|------|---------|-------|
| `path`         | string  | required | Absolute or repo-relative. Must resolve under project root (path-escape guard). |
| `mode`         | string  | `"auto"` | `"auto"` (pick by extension), `"cpp"`, `"py"`, `"md"`, `"json"`. Auto recognises `.cpp`/`.h`/`.hpp`/`.cc` → cpp; `.py` → py; `.md`/`.txt` → md; else byte-count fallback. |
| `include_doc_comment` | bool | `true` | When `true`, include the leading 1-30-line `//`/`#`/`"""` block at file top (file-level docstring). |
| `max_symbols`  | int     | `200`    | Cap on returned symbols (server-clamps to ≤ 1000). |

Response:

```jsonc
{
  "ok": true,
  "path": "src/auditdialog.cpp",
  "language": "cpp",
  "header_doc": "// auditdialog — static-analysis panel...",
  "symbols": [
    {"line": 412, "kind": "func", "name": "AuditDialog::runChecks", "signature": "void AuditDialog::runChecks()"},
    {"line": 587, "kind": "func", "name": "AuditDialog::renderHtml", "signature": "QString AuditDialog::renderHtml(...)"},
    {"line": 1043, "kind": "class", "name": "OutputFilter", "signature": "class OutputFilter : public QObject"},
    ...
  ],
  "truncated": false,
  "total_bytes": 267728,
  "total_lines": 5659
}
```

## 3. Implementation

**No ctags / no clangd / no tree-sitter.** Per Karpathy §2: a regex
scanner covers the 90% case in C++ / Python / Markdown.

**Regex set (all `static const QRegularExpression`, all with
`.optimize()` called once at first use; PCRE2 JIT enabled).** Each
input line is matched against the regexes in declaration order,
short-circuiting on the first hit. Lines > 1024 bytes skip regex
scan (legitimate decls don't exceed that; the cap blocks catastrophic
backtracking on minified / obfuscated input).

- **C++** (4 regexes):
  - `^[\w:]+\s+[\w:]+::[\w~]+\s*\(` (member def, qualified)
  - `^(class|struct|namespace)\s+\w+` (type decl)
  - `^(static|inline|template[^>]*>)?\s*[\w:<>&*\s]++\s++\w+\s*\([^)]*\)\s*[{;]`
    (free func; possessive quantifiers `+` to bound backtracking)
  - `^(signals|slots|public|private|protected|Q_(?:PROPERTY|INVOKABLE|DECLARE_\w+))\s*[:(]`
    (Qt-specific markers — recognised as `kind:"qt"`)
- **Python** (1): `^(async\s+)?(def|class)\s+\w+`.
- **Markdown** (1): `^(#{1,6})\s+(.+)$` → `kind:"heading"`,
  signature: full heading text including `##` prefix.
- **Other** (JSON / TOML / YAML / unknown): `language:"unknown"`,
  `symbols: []`, header_doc empty, `total_lines`/`total_bytes` set
  for orientation.

**Known regex misses (documented gaps):** lambdas, `Q_OBJECT` macro
expansions, generic-template `operator()` overloads, and any C++
construct broken across more than two lines without a continuation
indent. Acceptable for V1; covered by INV-3 (≥ floor).

**File-read strategy: stream line-by-line via `QFile::readLine()`.**
Picked over `QFile::map()` because (a) Qt's line-iterator handles
encoding cleanly via `QString::fromUtf8`, (b) memory stays bounded
even on a 100K-line file, (c) mmap on network-mounted homes is
fragile. Per-line cost target: **best-case typical ≤ 5 µs** (regex
JIT warm); **worst case** (a 1023-byte line consisting entirely of
chars from `[\w:<>&*\s]` with no `(` terminator): ≤ 30 µs on PCRE2
JIT 10.42, bounded by the per-line cap (INV-8) and possessive
quantifiers (no catastrophic backtracking). 5 000-line file =
≤ 25 ms typical / ≤ 150 ms worst-case adversarial; the latter is
deliberately uncached because the worst case is rare and we don't
warm-cache adversarial input.

`header_doc`: first 1-30 contiguous lines matching the file's
comment style (`//` for cpp, `#` for py, `<!--…-->` for md). Stops
at the first non-comment line. **Hard cap: total length ≤ 2 KiB**,
truncated with `…` marker. INV-9.

## 4. Invariants

- **INV-1.** `path` is resolved with the shared
  `pathInRepoRoot(userPath)` helper (introduced in ANTS-1248) —
  `QFileInfo::canonicalFilePath()` + NFC + reject byte < 0x20 + `\`,
  then `startsWith(projectRoot)`. On failure: `{ok:false,
  code:"bad_path"}`.
- **INV-2.** Empty `path` → `code:"bad_path"`. Non-existent path →
  `code:"not_found"`.
- **INV-3.** A C++ file with N top-level `void ClassName::method(`
  decls produces ≥ N `symbols[]` entries (no drop). Test fixture
  asserts a floor of ≥ 8 symbols on the in-tree `auditdialog.cpp`
  (known symbol count) — flips red if the regex set regresses.
- **INV-4.** Symbols are sorted ascending by `line`.
- **INV-5.** `truncated: true` iff there is at least one symbol
  beyond `max_symbols` that we did not return. Returning exactly
  `max_symbols` with no more available → `truncated: false`.
- **INV-6.** `header_doc` is empty string (not null) when the file
  has no leading comment block.
- **INV-7.** `language: "unknown"` files still emit `total_lines` /
  `total_bytes` (orientation use-case).
- **INV-8.** Regex set compiled once per process (`static const
  QRegularExpression` × 6) with `.optimize()` invoked at first use;
  no per-call compile. Lines > 1024 bytes skip regex scan.
- **INV-9.** `header_doc` capped at 2 KiB; longer extracted content
  is truncated with `…` suffix.
- **INV-10.** Reachability gate: same as ANTS-1248 INV-10 — UDS +
  MCP socket SO_PEERCRED gate. No new auth surface.

## 5. Memory & perf

- **Memory:** O(symbols × avg signature length) + 1 KiB per-line
  read buffer. At `max_symbols=200` × ~80 chars + buffer ≈ 17 KiB
  peak. `QFile::readLine()` keeps the file streaming, not loaded
  whole.
- **Per-call latency target:** ≤ 25 ms p50 on a 5 000-line C++ file
  (5 µs/line × 5 regex JIT-warm); ≤ 50 ms p99 cold. Single-pass.
- **No cache.** Same reasoning as ANTS-1248: cheap to recompute,
  caching adds invalidation complexity.

## 6. Token math

Against the four files above:

| File | Full Read | Outline | Compression |
|------|----------:|--------:|------------:|
| `claudeintegration.cpp` | ~17 K | ~1.3 K | ~13× |
| `terminalgrid.cpp`      | ~35 K | ~1.8 K | ~19× |
| `auditdialog.cpp`       | ~67 K | ~2.0 K | ~33× |
| `roadmapdialog.cpp`     | ~31 K | ~0.8 K | ~39× |

Typical "find function X" workflow: today Claude Reads the whole
file (~30 K tokens) to grep for X. With `file_outline` → ~1-2 K
tokens to see all symbols; pick the right line; then `Read` with
`offset`/`limit`. **~28 K saving per orientation pass.**

## 7. Files touched

| File | Change | Est. LoC |
|------|--------|---------:|
| `src/fileoutline.h` (new) | Header for the regex-scanner | ~30 |
| `src/fileoutline.cpp` (new) | C++ / py / md scanners | ~180 |
| `src/remotecontrol.h` | Public `cmdFileOutline(const QJsonObject &req)` | 1 |
| `src/remotecontrol.cpp` | Body (dispatch only — work in `fileoutline.cpp`) | ~25 |
| `src/claudeintegration.h` | setter + member | 2 |
| `src/claudeintegration.cpp` | tools/list + tools/call + setter body | ~25 |
| `src/mainwindow.cpp` | provider lambda | ~6 |
| `tests/features/mcp_file_outline/` (new) | spec.md + test_*.cpp | ~180 |
| `CMakeLists.txt` | Add `fileoutline.cpp` to main target + test wiring | ~3 |
| `ROADMAP.md` / `CHANGELOG.md` | ✅ + line | ~25 |
| **Total** | | **~477 LoC** (test + scanner-heavy) |

*Provider/setter/member triple here will be subsumed by ANTS-1253
(consolidation sweep noted in ANTS-1247 §2).*

## 8. Risks / mitigations

| Risk | Mitigation |
|------|-----------|
| Regex misses a signature shape and silently drops symbols | INV-3 floor + named known gaps (§3). Test fixture covers six common C++ decl shapes + Qt markers. |
| Multi-language project picks the wrong scanner via `auto` | Explicit `mode` param wins; auto only triggers on unambiguous extensions. |
| Path-traversal | Canonical-path startswith via shared helper (INV-1). |
| Symlink TOCTOU between resolve and open | Acceptable — single open follows resolve immediately; no fork window. Document. |
| Catastrophic regex backtracking on adversarial input | Per-line 1024-byte cap (INV-8) + possessive quantifiers (§3); PCRE2 JIT bounds the worst case. |
| Large files (>100 K lines) blow memory | Stream-read; never load full file into a single `QString` — read line-by-line. |
| Symbols cap hides real structure | `truncated: true` signals; caller can re-call with larger cap. |
| `header_doc` info-leak via huge banner | 2 KiB cap (INV-9). |

## 9. Out of scope (V1)

- **LSP / clangd integration** for semantic symbol resolution.
  Defer: order-of-magnitude more work; the regex set covers the
  90% orientation case.
- **Cross-file symbol references.** Different use-case (call-graph
  / xrefs); separate spec.
- **Full doc-string extraction.** V1 covers only the file-header
  comment block (orientation use-case); per-function docstrings
  would multiply token cost.
- **Languages outside cpp / py / md.** Falls back cleanly to
  `language:"unknown"` with `total_lines`/`total_bytes` populated;
  language-specific scanners land per ask.

Error codes follow `docs/standards/mcp-errors.md`: `bad_path`
(validation), `not_found` (state). Other tools' new codes also
follow the families documented there.

## 10. step → verify plan

1. `fileoutline.{h,cpp}` skeleton + regex scanners → verify: unit
   test in same file confirms cpp/py/md fixtures parse to expected
   symbol lists.
2. `cmdFileOutline` dispatch in `remotecontrol.cpp` → verify:
   `grep -c ANTS-1249-INV src/remotecontrol.cpp src/fileoutline.cpp` = 8.
3. MCP wiring (header + cpp + provider) → verify: `tools/list`
   includes `file_outline`.
4. Feature test pre-fix red, post-fix green (TDD).
5. Build + ctest green.
6. ROADMAP / CHANGELOG entries land.
