# ANTS-1299 — `build_status` MCP — cached build outcome + extracted errors

**Status:** spec draft (2026-05-19).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-1299 (mcp-general-claude-code-workflows-2026-05-13,
pull 34 of the post-1308 bundle plan).
**Pairs with:** ANTS-1300 (`test_results` — sibling tool, same `.audit_cache/`
cache directory, same record/read op shape). ANTS-1555 (`.audit_cache/`
infrastructure — re-uses `AuditCache::cacheDir`).

## 1. Problem

Every multi-step Claude task does build cycles. Today Claude shells
`cmake --build` and reads ~50–500 lines of compiler output to decide
whether to continue. The token cost is real and recurring:

- Default `cmake --build build 2>&1 | tail -20` (per CLAUDE.md's
  token-frugal pattern) emits ~600–4 000 bytes of output per call.
- On a multi-error compile, the relevant `error:` lines are buried
  among ~10–100× as much warning / progress / link-line chatter.
- Across context-compacts, the same build output is re-read by a
  later session that picks up the work.

The data is structured and re-derivable — file:line:severity:message
— but Claude pays the framing tax each time. Caching the parsed
outcome at `<project>/.audit_cache/build.json` lets a later read
return the compact summary (typically ~200–1 500 bytes) instead of
re-shelling the build or re-paging the scrollback.

## 2. Goal

`build_status` returns the most-recent recorded build's
`{exit_code, errors:[…], errors_count, warnings_count}` envelope
without re-running the build. Claude records the outcome via
`op=record` after each build; subsequent sessions / tabs / agents
read via the default `op=read`.

## 3. Surface

### 3.1 Tool wiring

- **Name:** `build_status`.
- **Surface:** MCP-only. Registered via `registerToolProvider` in
  `MainWindow::setupClaudeMcpProviders`. No new `cmd ==` branch in
  `RemoteControl::dispatch`. Rationale: the callers are Claude
  sessions speaking MCP; non-MCP `--remote` callers can compose
  upstream by reading `.audit_cache/build.json` directly.
- **Caller-cwd contract:** Required. Both `op=record` and `op=read`
  scope to the caller's project root. `RcGate` is not needed —
  the file at `<root>/.audit_cache/build.json` is per-cwd by
  construction, and the only attack surface is "caller corrupts
  their own cache", which is not a privilege escalation.
- **Kind bucket:** `[build]` in `kindForName`.
- **Etag:** the dispatcher's `applyEtagPattern` is op-agnostic — it
  injects an `etag` field on every JSON response from any tool
  listed in `isEtagSupportedTool`. `op=record` responses therefore
  also carry an etag, but it is not *useful* (the record body
  changes between calls by definition). Callers should pass
  `etag_match` only on `op=read`; passing it on `op=record` is
  harmless (the hashes won't collide except by coincidence) but
  not semantically meaningful.
- **Rate-limit class:** Cheap (default). The verb is a structural
  parser + atomic-write or a single file read.

### 3.2 Cache layout

`<project>/.audit_cache/build.json` — single-file JSON document,
not a directory. v1 schema:

```json
{
  "schema_version": 1,
  "recorded_at_ms": 1747681920000,
  "exit_code": 0,
  "started_at_ms": 1747681900000,
  "finished_at_ms": 1747681920000,
  "duration_ms": 20000,
  "errors": [
    {"file": "src/foo.cpp", "line": 42,  "severity": "error",
     "message": "no matching function for call to 'bar(int)'"}
  ],
  "errors_count": 1,
  "errors_truncated": false,
  "warnings_count": 3,
  "output_byte_size": 4823
}
```

Field guarantees:

- `schema_version` is always `1` for this release. Loaders that see
  any other value treat the cache as absent (returns
  `{ok:false, code:"not_cached"}` on `op=read`).
- `recorded_at_ms` is the wall-clock ms when `recordBuild` writes
  the file (Ants-side, not caller-side). Always present.
- `exit_code` is the integer the caller passed in. No coercion.
- `started_at_ms` / `finished_at_ms` / `duration_ms` are echoed
  verbatim when the caller supplied them, omitted otherwise. The
  cache is shape-stable: missing inputs are absent fields, not
  zero/null.
- `errors[]` is capped at 50 entries (v1). When the parser
  extracts > 50, `errors_truncated:true` is set and the first 50
  are retained (parse order). `errors_count` always reflects the
  total number of parsed errors, including those dropped by the
  cap.
- `warnings_count` is the count of parsed `warning:` lines. No
  body excerpt is kept for warnings in v1 — the count is the
  whole-of-purpose signal ("did the build emit warnings the user
  may want to flag?").
- `output_byte_size` is the byte length of the caller-supplied
  output the parser was fed.

### 3.3 Op surface

#### `op=record`

```json
{
  "op": "record",
  "caller_cwd": "/abs/path",
  "exit_code": 0,
  "output": "<full stdout+stderr text of the build>",
  "started_at_ms": 1747681900000,   // optional
  "finished_at_ms": 1747681920000   // optional
}
```

Server-side flow:

1. Validate `caller_cwd` (Required contract).
2. Validate `exit_code` is an integer (required for record), and
   `output` is a non-empty string (required for record). Missing
   or wrong-typed → `{ok:false, code:"bad_args"}`. `op` values
   outside `{record, read}` also refuse with `bad_args`. Empty
   `output` refuses with `bad_args` — a successful build still
   emits link lines, so the empty case is always a caller bug.
3. Parse `output` into errors[] + warnings_count via the GCC /
   clang regex (see § 3.4 for the full pattern set). Cap
   errors[] at 50.
4. Build the response envelope per § 3.2.
5. Write `<root>/.audit_cache/build.json` atomically via
   `QSaveFile`, perms 0600 via `setOwnerOnlyPerms`.
6. Return the same envelope (with `ok:true` added).

#### `op=read` (default)

```json
{
  "op": "read",   // optional; default
  "caller_cwd": "/abs/path",
  "etag_match": "<optional, ANTS-1499 304 pattern>"
}
```

Server-side flow:

1. Validate `caller_cwd` (Required contract).
2. Load `<root>/.audit_cache/build.json`. If absent, malformed, or
   `schema_version != 1` → `{ok:false, code:"not_cached"}`.
3. Walk the project's compile-input set looking for any file with
   `mtime_ms > recorded_at_ms`. The set is: `<root>/src/`,
   `<root>/tests/`, `<root>/cmake/` (each if it exists), plus
   `<root>/CMakeLists.txt` (always probed when present). Third-
   party headers under `/usr/include/`, generated files under
   `<root>/build*/`, and the `.audit_cache/` directory are
   deliberately excluded — they are not compile *inputs*. First
   match sets `stale:true`. Walk is capped at 5 000 files; if cap
   hit before any newer file is found, `stale_walk_capped:true`
   is set and `stale` is omitted (not false — distinguishes
   "looked, none found" from "could not walk fully"). Future-
   proofing: if a project carries a top-level `include/` directory,
   it is also probed (Ants itself does not, today).
4. Return the cached envelope with `ok:true` injected.

The dispatcher's `applyEtagPattern` runs after the handler — adds
the `etag` field and short-circuits to `{ok:true, unchanged:true,
etag:"<same>"}` when `etag_match` equals the freshly-computed hash.

### 3.4 Error-line parser

The parser is intentionally a small grep, not a full GCC-output
state machine. Recognised forms:

- `<file>:<line>:<col>: error: <message>` — gcc/clang default.
  This regex (with `<col>` optional) also matches cppcheck 2.x
  default stdout, which adopted the gcc-compatible shape years
  ago; the project's own audit pipeline ingests cppcheck via XML
  (see `src/auditengine.cpp`), so cppcheck-stdout parsing here
  is a thin best-effort path, not the authoritative cppcheck
  surface.
- `<file>:<line>:<col>: warning: <message>` — gcc/clang default.
- `<file>:<line>:<col>: note: <message>` — handled as a
  continuation of the immediately-preceding `error:` line (folded
  into the parent's message) OR as a standalone diagnostic that
  bumps `warnings_count`. See the continuation rule below for the
  disambiguation.
- `<file>:<line>: error: <message>` (no column) — make / ld /
  some clang front-ends.
- `<file>:<line>: warning: <message>`.

Lines that do not match are ignored. **Continuation rule:** a
`note:` line that immediately follows an `error:` line (no blank
line, no intervening non-matching text) is appended to the
parent's `message` with a `" / "` separator and is NOT counted in
`warnings_count` (one diagnostic, not two). A `note:` line that
appears in isolation (after a blank line, after a `warning:` line,
or after a non-matching line) is counted as a warning. Per-message
text is capped at 240 chars total, regardless of the number of
continuations folded in.

### 3.5 Refusal codes

All codes below are in
[`docs/standards/mcp-error-codes.md`](../standards/mcp-error-codes.md).
ANTS-1299 + ANTS-1300 add `not_cached` and `detail_not_found`
(ANTS-1300 only) to Category 2 (Resource state) in the same commit
that ships the implementations.

- `caller_cwd_required` — Required-contract gate (dispatcher).
- `bad_args` — `op=record` without `exit_code`/`output`, wrong
  type, empty `output`, or `op` value outside `{record, read}`.
- `not_cached` — `op=read` and no cache file exists (or it failed
  schema check). Not an error envelope in the operator-fault sense
  — surfaced as `{ok:false, code:"not_cached", error: "no recorded
  build"}` so callers can branch on the code without parsing the
  message.
- `no_project` — `resolveRootCanonical` returned empty.
- `write_failed` — `op=record` and atomic write failed.

## 4. Invariants

| # | Statement |
|---|-----------|
| 1 | `BuildCache` namespace in `src/buildcache.{h,cpp}` exposes `cachePath(rootCanonical)`, `recordBuild(rootCanonical, ParsedBuild)`, `loadBuild(rootCanonical) → optional<ParsedBuild>`, and `parseBuildOutput(output) → ParsedBuild`. No GUI deps. |
| 2 | Cache file is `<root>/.audit_cache/build.json`. Same `.audit_cache/` layout as `AuditCache` writes (so `last_audit_summary` and `audit_run` consumers see it on disk), but the directory path is computed by a one-line inline helper in `buildcache.cpp` rather than calling `AuditCache::cacheDir` directly — keeps `buildcache` off the `ants_audit_lib` link path so it can live in `ants_core_lib` alongside the dispatcher in `remotecontrol.cpp`. |
| 3 | `recordBuild` writes via `QSaveFile` (atomic) with 0600 perms via `setOwnerOnlyPerms`. Returns false on write failure; caller surfaces `code:"write_failed"`. |
| 4 | `loadBuild` returns empty on missing file, parse failure, OR `schema_version != 1`. No throw, no qWarning spam beyond the initial parse-error log. |
| 5 | `parseBuildOutput` recognises GCC/clang `path:line:col?:` lines (the same regex matches cppcheck 2.x default stdout per § 3.4; the legacy `[file:line]: (sev)` cppcheck-1.x bracket shape is NOT supported in v1). Errors capped at 50; `errors_truncated:true` when truncated. Warnings counted in total, no body kept. |
| 6 | `cmdBuildStatus(const QJsonObject &req) -> QJsonDocument` declared public on `RemoteControl` in `remotecontrol.h` next to `cmdSpecQuery`. Return type matches every other `cmd*` tool provider (`cmdSpecQuery`, `cmdCurrentState`, etc.); the registration lambda calls `.toJson(QJsonDocument::Compact)` on the result. |
| 7 | `cmdBuildStatus` defined in `remotecontrol.cpp` with an `ANTS-1299` anchor comment in or above the body. Dispatches on `op` ∈ `{read, record}` (default `read`). |
| 8 | `op=record` validates `exit_code` (integer) and `output` (non-empty string) before parsing. Missing / wrong-typed / empty `output` → `{ok:false, code:"bad_args"}`. `op` values outside `{record, read}` also refuse with `bad_args`. |
| 9 | `op=read` returns `{ok:false, code:"not_cached"}` when no cache exists or it fails schema check. Never returns `ok:true` with a synthetic empty envelope. |
| 10 | `MainWindow::setupClaudeMcpProviders` registers `"build_status"` via `registerToolProvider` and delegates to `m_remoteControl->cmdBuildStatus`. Falls back to the mainwindow-local `kRcUnavailable` constant (`mainwindow.cpp`) when `m_remoteControl` is null. |
| 11 | `claudeintegration.cpp` tools/list block registers `"build_status"` with `caller_cwd` (required at JSON-schema level), `op` (optional), `exit_code` (optional), `output` (optional), `started_at_ms` (optional), `finished_at_ms` (optional), `etag_match` (optional). The record-mode args are schema-optional but conditionally required at runtime when `op=record`; missing them surfaces `bad_args` from the handler, not a schema error. No other input properties. |
| 12 | `callerCwdContractFor` classifies `"build_status"` as `Required` (explicit branch). |
| 13 | `kindForName` classifies `"build_status"` as `"build"` — a new bucket branch added to the lambda, before the fall-through default. |
| 14 | `isEtagSupportedTool` returns true for `"build_status"`. Etag injection is op-agnostic (dispatcher contract), so `op=record` responses also carry an `etag` field, but the field is only semantically useful on `op=read`. |
| 15 | Token-cost ledger in `claudeintegration.cpp` carries `{QStringLiteral("build_status"), {500, 2000}}` (typical, cap) — matches the `QStringLiteral` style of every existing entry. |
| 16 | `op=read` walks the project's compile-input set (`src/`, `tests/`, `cmake/`, `CMakeLists.txt`, plus `include/` if present) to detect stale cache. Walk capped at 5 000 files; `stale_walk_capped:true` surfaced when cap hit (and `stale` omitted); `stale:true` surfaced on first newer mtime hit. Excludes `/usr/include/`, `build*/`, `.audit_cache/`. |

## 5. Memory & cost budget

- Per call: one `QJsonDocument` + one `QFile`. Cache file is ≤ ~50 KiB in the worst case: 50 errors × (240-char `message` cap + ~240 bytes of envelope per entry, dominated by absolute path + JSON quoting + structural keys). The 240-char message cap is the load-bearing constant; continuations folded into one diagnostic share that budget.
- Stale-check walk: bounded by 5 000-file cap; on this project's `src/` (~150 files) the walk completes in < 5 ms.
- Atomic write fits in one `QSaveFile` commit; no risk of partial files on crash.
- No new in-process state. The cache is read-on-demand each call (no in-memory mirror).

## 6. Out of scope

- Running the build server-side. v1 is record-after-build only; Claude or CI still drives the actual `cmake --build`.
- A `op=clear` op (delete the cache file). Re-recording with a fresh `exit_code:0` overwrites; that's enough.
- Per-target / per-preset caches. The cache is per-project, one slot. Re-recording a debug build overwrites the release build's slot. v2 may key by preset.
- Multi-tool build parsers (Cargo, Go, MSBuild, etc.). v1 is GCC/clang/cppcheck only — the Ants project's own toolchain.
- Cache invalidation by content hash. v1 uses mtime; mtime stamps are sufficient for the "did I edit since the last build?" question.
- Streaming progress events while a build runs. The MCP-only surface here is post-hoc record; live progress is ANTS-1443's lane.

## 7. Doc-drift fold-in (apply in the same commit)

- Flip `ROADMAP.md` ANTS-1299 to ✅ and update its envelope sketch (currently `{exit_code, started_at, finished_at, errors: […], warnings_count}`) to match the shipped shape (suffix `_ms` on timestamps, plus the `recorded_at_ms` + `output_byte_size` + `errors_count` fields).
- Add `not_cached` to `docs/standards/mcp-error-codes.md` § 2 (alongside ANTS-1300's `detail_not_found`).
- Add a CHANGELOG entry to the unreleased section.
