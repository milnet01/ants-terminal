# Feature spec: ANTS-1397 v1 — `test_audit_*` MCP trio

v1 ships the four-verb engine + MCP wiring. Hardcoded pre-pass
patterns + shallow mtime recheck + missing drift-guard test land
in v2 (ANTS-1450 follow-up).

## Invariants exercised by this test set

- **INV-1 / no-shell partition + pre-pass.** Source-scrape:
  partition body has no `QProcess` instantiation.
- **INV-2 / brief is path-only, never bodies.** Source-scrape:
  no `readAll` from chunk source files in `brief()`.
- **INV-3 / fold-in delegates to RoadmapFoldIn.** Source-scrape:
  `foldIn()` calls `RoadmapFoldIn::allocateIds(canon, n)` (ONE
  call with N) and `RoadmapFoldIn::insertBlock(...)` (ONE call).
  No `QSaveFile` / `.roadmap-counter` access in
  `testauditengine.cpp`.
- **INV-4 / token via qHash, not SHA-256.** Source-scrape:
  `qHash(callerCwd)`, `qHash(scope)` present; no
  `QCryptographicHash::Sha256` in testauditengine.cpp.
- **INV-5 / all four verbs Optional contract.** Source-scrape on
  claudeintegration.cpp: none of the four test_audit verbs appear
  in the Required branch.
- **INV-6 / dimension list canonical + injected to brief.**
  `kDimensions` static QStringList declared; `brief()` populates
  the response with `p->dimensionsActive`.
- **INV-7 / pre-pass per-chunk cap 20.** `kPrePassPerChunkCap = 20`.
- **INV-8 / synth fences per-chunk reports.** `synthesize()` emits
  `<chunk_report file="…">…</chunk_report>` around report content;
  escapes nested fence markers.
- **INV-9 / chunk size clamped [4, 30].** `kChunkSizeMin = 4`,
  `kChunkSizeMax = 30`.
- **INV-10 / pagination via offset/limit/truncated/next_offset.**
  partition envelope carries those four fields when caller paged.
- **INV-13 / pre_pass_findings have NO matched text.** The
  `prePassFile` JSON shape only sets `{file, line, pattern_id,
  dimension}` — never the matched line text.
- **INV-14 / path-typed args validated.** `caller_cwd` /
  `reports_dir` / `scope:"path:"` / `scope:"files:..."` all
  validated via `canonicalFilePath` + isDir checks.
- **INV-15 / mtime-walk cache + 5 s recheck rate-limit.**
  `kMtimeRecheckRateLimitMs = 5000`.
