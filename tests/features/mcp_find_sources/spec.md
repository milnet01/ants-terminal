# Feature test — `find_sources` MCP topic-to-files discovery (ANTS-1636)

Contract for the `FindSources` pure-function unit + its MCP wiring.

## What this test locks

**Pure-function `FindSources::findSources` behaviour:**

- **INV-1** — `tokenise` splits on whitespace + common punctuation,
  drops tokens shorter than 3 chars, drops `ANTS-NNNN` roadmap-id
  tokens (they shouldn't be searched literally in source).
- **INV-2** — `variantsForToken` produces lowercase + snake_case +
  camelCase + dropped-separator variants. De-duplicated.
- **INV-3** — `findSources` returns top N files by score; ties broken
  by path ascending for stable ordering. Default cap 20, hard-cap 100.
  `truncated` is true iff the pre-cap result list was longer than the
  cap.
- **INV-4** — Per-file content scan reads at most `contentByteCap`
  bytes (default 256 KiB) — the search is "where might this be", not
  exhaustive grep.
- **INV-5** — Filename match counts each input token at most once
  regardless of how many variants of that token appear in the
  filename (so a four-token topic with one filename hit scores 50, not
  50 × variants).
- **INV-6** — Generated files (`moc_*`, `ui_*`, `qrc_*`,
  `*.pb.{cc,h}`, `*_generated.{cpp,h}`, `/generated/` paths) are
  skipped.
- **INV-7** — Files with zero hits across all tokens are dropped from
  the result; tokens with zero hits across all files appear in
  `unmatchedTerms`.
- **INV-8** — Role classification: `tests/` paths → "test", `.h`/
  `.hpp` → "header", otherwise → "impl".

**Wiring contract** (source-grep):

- **INV-9** — `remotecontrol.h` declares `cmdFindSources`;
  `remotecontrol.cpp` defines `RemoteControl::cmdFindSources`.
- **INV-10** — `mainwindow.cpp` registers `find_sources` via
  `registerToolProvider` with `CallerCwdContract::Required`.
- **INV-11** — `claudeintegration.cpp` carries the tool descriptor,
  the token-cost entry, the `"workspace"` `kindForName` bucket
  membership, and the `Required` `callerCwdContractFor` branch.
- **INV-12** (ANTS-3415) — `symbol` is accepted as an alias for
  `topic`: the handler falls back to `req.value("symbol")`, and the
  schema declares a `symbol` prop so it isn't flagged in
  `ignored_args`.
- **INV-13** (ANTS-3435) — an empty result (`files.isEmpty()`) carries
  a redirect `hint` (anchored to ANTS-3435) that names the exact-match
  verbs — `workspace_search` / `find_definition` / `find_caller` — so a
  caller doesn't read `files_count:0` as a genuine "no such code".

**Pre-warm** (ANTS-3444a):

- **INV-14** — `FindSources::prewarm(root)` warms the OS page cache for
  exactly the `findSources` candidate set: it shares the
  `collectCandidates` walk with `findSources`, so the count it touches
  equals `findSources(root).filesScanned` for the same root, is `> 0`
  for a real tree, and is `0` for a bad/rootless path (no crash). It
  reads only (one reused buffer, no in-process cache), so its sole
  residency is the OS page cache. `session_orient` launches it on a
  background thread once per project root per session.

Exit 0 = every invariant holds.
