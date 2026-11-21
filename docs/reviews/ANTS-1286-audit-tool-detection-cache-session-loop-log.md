# ANTS-1286 — Audit tool-detection cache (session-scoped) — review loop log

Record for [`../specs/ANTS-1286-audit-tool-detection-cache-session.md`](../specs/ANTS-1286-audit-tool-detection-cache-session.md) § 9. Cold-eyes loop.

## Cold-eyes loop log

### Loop 1 (self)

- [x] INV consistency: INV-1 says "zero I/O on cache hit" — fine.
      INV-7 says we use `QStandardPaths::findExecutable` which IS
      I/O (it `stat`s candidates in PATH). Resolved: INV-1 applies
      to **cache hits**, INV-7 describes the **cold path**. Both
      consistent.
- [x] Cross-doc: no doc currently claims tool detection is
      cached. Greped ROADMAP.md / CHANGELOG.md / audit specs for
      stale claims — none.
- [x] RAM budget stated (§ 6) — ≤ ~10 KiB.
- [x] No duplication with existing engines. Greped
      `QStandardPaths::findExecutable` across `src/` — 5 call-sites
      total: `mainwindow.cpp:5121, 5289, 5291`, `terminalwidget.cpp:3700, 3702`.
      None are in the audit hot path. Could fold into the engine
      later (rule of three) but out of scope for v1.
- [x] Build cost: one new TU compiled into `ants_core_lib`. Header
      pulls `QHash`, `QString` — already widely included.

### Loop 2 (Phase-3 verified against as-shipped code)

- [x] `AuditDialog::toolExists` signature (one `QString` arg,
      returns `bool`) preserved at `auditdialog.cpp:349`.
- [x] `QStandardPaths::findExecutable` proven a known idiom in
      this codebase (`mainwindow.cpp`, `terminalwidget.cpp`).
- [x] `ants_core_lib` already has Qt6::Core dep — adding
      `QStandardPaths` is free.
- [x] Engine pattern matches `SessionMemoryEngine` /
      `TokenUsageEngine` (no widget linkage; lives in
      `ants_core_lib`).
- [x] Test bundle: `test_audit` builds `SessionMemoryEngine` tests
      — extends to `ToolDetectionEngine` symmetrically.
- [x] Shipped `tooldetectionengine.cpp:18` uses `qgetenv("PATH")`
      (returns `QByteArray`). The § 4 implementation sketch
      matches.
- [x] Shipped `auditdialog.cpp` body of `toolExists` is a one-line
      forward to `ToolDetectionEngine::exists`, with no `QProcess`.
      INV-7 + INV-8 hold.
- [x] § 1 tool-count table: cppcheck appears at `auditdialog.cpp:1128`
      (twice in same expression) and `1150` — 3 occurrences total.
      Aggregate `grep -c "toolExists" auditdialog.cpp = 27` lines.
      Real call-site count ≈ 25 (minus the def + the forward).

### Loop 3 (cold-eyes review of this spec by independent reviewer, 2026-05-14)

Findings verified and fixed in-place:

- **H1 (fixed)** — § 9 had a wrong citation:
  `audithygiene.cpp:160` does not contain `findExecutable`; that
  line is `out << QStringLiteral("qt6");`. Replaced with the actual
  call-sites (`mainwindow.cpp:5121, 5289, 5291`,
  `terminalwidget.cpp:3700, 3702`).
- **H2 (fixed)** — § 4 sketch used `qEnvironmentVariable("PATH")`
  but shipped engine uses `qgetenv("PATH")`. Reconciled to the
  shipped form (raw bytes, no codec round-trip).
- **H3 (fixed)** — § 1 "30+ times" headline replaced with the
  literal `grep -c = 27` count, with a row explaining
  `addToolCheck:391`'s re-probe semantics. Practical
  count ≈ 25 probes.
- **M1 (fixed)** — § 1 cppcheck row corrected from
  "2 (lines 1129, 1129)" to "3 (lines 1128 ×2 + 1150)".
- **M2 (fixed)** — INV-9 + § 6 now state the per-entry overhead
  range (120–200 B) instead of a flat ~200 B.

Open questions deferred (acknowledged, not blocking):

- INV-5 (thread-safety) has no dedicated test (TDE-9). Mutex
  presence is verified by code review — empirical race testing
  needs `QtConcurrent::run` and yields probabilistic coverage.
  Acceptable for v1.
- INV-6 (no-disk-IO) — covered by code review; a grep test against
  `tooldetectionengine.cpp` for `QFile`/`QSaveFile` strings is
  trivially-passing but adds noise. Skipped.
- INV-10 (16-hex-truncation collision-negligibility) — design
  claim, not behaviour to test. Distinct PATHs producing distinct
  hashes is implied by SHA-256; not asserted.

**Loop 3: CLEAN** after Phase-3 fixes. Spec is implementer-ready.
