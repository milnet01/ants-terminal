# `audit_run` per-project `.audit_cache/` infrastructure (ANTS-1555)

Feature-conformance spec for the runtime + envelope contract that
routes `audit_run`'s SARIF output (and manifest) into the same
`<project>/.audit_cache/` directory the AuditDialog GUI already
uses, replacing the per-call `/tmp/audit-<sid>-<seq>.sarif` path.

This is a source-scrape + lightweight in-process exercise of the
`AuditCache` module — the heavy end-to-end `runAudit` path needs
external tools on PATH (cppcheck, ruff, etc.) and is exercised by
`mcp_audit_run`. Here we lock the wiring + manifest behaviour.

Full spec: `docs/specs/ANTS-1555.md`.

## Invariants exercised

- **INV-1** — `runAudit` reaches `AuditCache::sarifPathFor` + writes
  SARIF under `<root>/.audit_cache/`, not `/tmp/`, on the success
  path. Source-scrape against `auditrunner.cpp`.
- **INV-2** — `RunResult` declares `cachePath` + `priorRun` fields.
  Source-scrape against `auditrunner.h`.
- **INV-3** — Manifest history is capped at 10 entries by
  `recordRun`. In-process test: pre-write a manifest with 11
  entries, call `recordRun` once, re-load → `history.size() == 10`.
- **INV-4** — The reaper deletes only sarif/html files named in
  dropped history entries. In-process test: pre-write a manifest
  with 11 history entries + corresponding sarif files + a foreign
  `audit-foreign.sarif`. After reap, the foreign file survives;
  the dropped entry's file is gone.
- **INV-4a** (ANTS-2004) — The reaper runs only *after* the new
  manifest is durably committed (`QSaveFile::commit()` +
  `fsyncParentDir`). A commit failure returns early with the old
  files intact, so the manifest and filesystem never disagree.
  Source-scrape: `sf.commit()` precedes the reaper loop in
  `auditcache.cpp`. Remove failures are surfaced via `qWarning`.
- **INV-6** — `recordRun` writes manifest with 0600 perms via
  `setOwnerOnlyPerms` + `QSaveFile`. Source-scrape:
  `auditcache.cpp` uses both helpers.
- **INV-7** — Unknown manifest version → empty Manifest. In-process
  test: pre-write `index.json` with `version: 99`; `loadManifest`
  returns `lastRun.isEmpty()` true.
- **INV-8** — `priorRun` reflects the prior manifest's last_run.
  In-process test: pre-write manifest with a known last_run; call
  `recordRun`; `priorRunOut` matches the pre-written last_run
  verbatim (iso_timestamp + commit fields).
- **Envelope** — `audit_run` MCP provider emits `cache_path` +
  `prior_run` fields when set. Source-scrape against
  `mainwindow.cpp`.
- **Descriptor** — `audit_run` description mentions
  `.audit_cache/` and `cache_path`. Source-scrape against
  `claudeintegration.cpp`.

## Out of scope

- ANTS-1504 (since-last-run mode) — separate spec, lands on top of
  this infrastructure.
- End-to-end `runAudit` invocation with real tools — covered by
  `mcp_audit_run`.
- Cross-tool SARIF body validation — covered by `mcp_last_audit_summary`.
