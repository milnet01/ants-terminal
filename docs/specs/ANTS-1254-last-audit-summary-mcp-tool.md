# ANTS-1254 — `last_audit_summary` MCP tool

**Status:** spec, ship-ready after 5-loop cold-eyes pass
(2026-05-13). Loop-by-loop fixes documented at end of file (§ 11).
**Source:** cold-eyes pass 3 finding on the 1247-1252 token-reduction
pack, 2026-05-12.
**Blocked by:** ANTS-1253 (uses the consolidated
`registerToolProvider` registry from day one — no per-tool setter or
member here).
**Siblings:** ANTS-1247, 1248, 1249, 1250, 1251, 1252, 1253.

## 1. Problem

`/audit` runs persist two artefacts under
`{projectPath}/.audit_cache/`:

- `audit-YYYYMMDD-HHmmss.sarif` — machine-readable, typically
  50-500 KiB (SARIF v2.1.0; mode 0600 — see `auditdialog.cpp:3357-3367`).
- `audit-YYYYMMDD-HHmmss.html` — single-file HTML report.

When Claude (or any MCP consumer) needs to consult "what's broken
right now?", today's flow is `Read` of the HTML report → ~5-15 K
tokens. Most of that is CSS, chrome, JS, and per-finding boilerplate.
The SARIF file would be smaller per finding but is itself ~150-2000
findings × ~500B JSON = also too large to read whole.

A read-only MCP tool that opens the *latest* SARIF on disk and
returns a compact summary:

```jsonc
{
  "ok": true,
  "run_at": "2026-05-12T08:13:00Z",
  "sarif_path": "/abs/.audit_cache/audit-20260512-081300.sarif",
  "html_path":  "/abs/.audit_cache/audit-20260512-081300.html",
  "counts": {"error": 2, "warning": 21, "note": 23, "suppressed": 4},
  "top_findings": [
    {"level":"error", "severity":"CRITICAL",
     "ruleId":"clazy-no-trivial-copyable-in-list",
     "file":"src/foo.cpp", "line":42, "message":"…",
     "confidence":85, "highConfidence":true}
  ]
}
```

…is ~200-400 tokens depending on `top_n`. **Saving per audit
consultation: ~5-15 K tokens.**

## 2. Surface

| Field | Type | Default | Notes |
|-------|------|---------|-------|
| `top_n`     | int    | `5`  | Cap on `top_findings[]`. Server-clamp `[0, 50]`. |
| `severity_floor` | string | `"warning"` | `"error" / "warning" / "note"` — **SARIF level names**, matching the on-disk `runs[].results[].level` field. Findings *below* this floor are excluded from `top_findings[]` but always counted in `counts`. |

**Why SARIF level names, not `critical/high/medium/low/info`?** The
SARIF file already carries `runs[].results[].level` ∈
`{"error","warning","note"}` — taking that as the wire vocabulary
needs no mapping at read time and no doc-vs-file drift risk. The
finer-grained internal taxonomy (Info/Minor/Major/Critical/Blocker)
is preserved per-finding via `top_findings[].severity`
(`properties.severity` in SARIF; one of those five strings) — callers
that need it can sort/filter on it.

### 2.0 Level ordering (canonical)

The `severity_floor` comparator and INV-3 sort use the following
ordinal map. Implementation lives in a single helper to avoid drift:

| `level` string | Ordinal | Notes |
|----------------|--------:|-------|
| `"error"`      | 2 | Highest. SARIF-emitted from internal Critical/Blocker. |
| `"warning"`    | 1 | Default when `level` field absent on a result. |
| `"note"`       | 0 | Lowest. SARIF-emitted from internal Info. |

`level >= levelFloor` and "error > warning > note" elsewhere in the
spec are shorthand for "compare ordinals from this table."

### 2.1 Response — success

```jsonc
{
  "ok": true,
  "run_at":     "<ISO-8601 UTC, runs[0].invocations[0].startTimeUtc>",
  "sarif_path": "<absolute path to selected .sarif>",
  "html_path":  "<sibling .html if present, else \"\">",
  "counts": {
    "error":      <int>,   // SARIF level
    "warning":    <int>,
    "note":       <int>,
    "suppressed": <int>    // findings carrying SARIF suppressions[]
  },
  "top_findings": [
    {
      "level":          "error" | "warning" | "note",
      "severity":       "BLOCKER" | "CRITICAL" | "MAJOR" | "MINOR" | "INFO",
      "ruleId":         "<check id>",
      "file":           "<artifactLocation.uri, possibly empty>",
      "line":           <int, 0 if absent>,
      "message":        "<results[].message.text>",
      "confidence":     <int 0..100, -1 if absent>,
      "highConfidence": <bool>
    }
  ]
}
```

### 2.2 Response — error envelope

```jsonc
{ "ok": false, "code": "<family>", "error": "<human-readable>" }
```

Codes (per `docs/standards/mcp-errors.md`):

| Code | Family | When |
|------|--------|------|
| `not_audited`        | `not_<thing>` (state) | No `.audit_cache/` dir, or no `audit-*.sarif` files inside. |
| `bad_severity_floor` | `bad_<arg>` (input) | `severity_floor` not in `{error,warning,note}`. |
| `parse_failed`       | `<thing>_failed` (tool) | SARIF body present but not valid JSON / missing `runs[0].results`. **No `stderr` field** — SARIF is self-emitted (no external producer). The `<thing>_failed` family permits `stderr` (mcp-errors.md:84) but does not mandate it. |
| `read_failed`        | `<thing>_failed` (tool) | SARIF file open() / read() failed (permissions, EIO). No `stderr` (same reason). |

## 3. Implementation

### 3.1 New: `AuditEngine::summariseSarif`

```cpp
// auditengine.h — additions

// Wire-shape view of a single SARIF finding for the
// last_audit_summary tool. Distinct from `Finding` (the audit
// dialog's in-memory parse target) because the wire needs the
// SARIF `level` string AND the resolved 5-level `severity`
// string, neither of which `Finding` carries (it has only the
// `Severity` enum). Keeping a separate struct avoids retrofitting
// the dialog's data model.
struct AuditSummaryFinding {
    QString level;           // "error" / "warning" / "note"
    QString severity;        // "BLOCKER" / "CRITICAL" / "MAJOR" / "MINOR" / "INFO"
    QString ruleId;
    QString file;            // SARIF artifactLocation.uri (as-is; INV-7)
    int     line          = 0;
    QString message;
    int     confidence    = -1;
    bool    highConfidence = false;
};

struct AuditSummary {
    QString sarifPath;        // absolute
    QString htmlPath;         // absolute, "" if no sibling within ±60 s
    QString runAtIso;         // empty if SARIF lacks invocations
    int     countError      = 0;
    int     countWarning    = 0;
    int     countNote       = 0;
    int     countSuppressed = 0;
    QList<AuditSummaryFinding> topFindings;  // already-trimmed, already-sorted
};

namespace AuditEngine {
// Reads the SARIF at `sarifPath` (must exist), returns the compact
// summary. Caller decides path discovery + caching. Pure parser:
// no Qt6::Widgets, no I/O outside the read.
//
// `topN` is server-clamped by the caller (this fn trusts the input).
// `levelFloor` ∈ {"error","warning","note"} (caller-validated).
std::optional<AuditSummary> summariseSarif(
    const QString &sarifPath,
    int topN,
    const QString &levelFloor);
}  // namespace AuditEngine
```

Body (sketch):

1. `QFile::open(ReadOnly)` → on failure return `nullopt`.
   The caller maps that to `read_failed`.
2. `QJsonDocument::fromJson` → on parse error return `nullopt`. The
   caller distinguishes parse vs read by checking
   `QFile::exists()` + reading size first; alternative is two
   wrappers. Picked: `summariseSarif` returns `nullopt` and the
   caller in `remotecontrol.cpp` knows the discovery succeeded so
   the failure must be parse.
3. **Build the rule index first.** Walk
   `runs[0].tool.driver.rules[]` and key `properties.severity`
   (uppercase string — same 5 labels emitted by
   `auditdialog::severityLabel` at `auditdialog.cpp:4062`) by
   `rule.id`. The engine carries its own copy of the
   enum→string map because `severityLabel` in the dialog is `static`
   (file-local) and not callable from `auditengine.cpp`. This is the
   only place severity lives in current emit
   (`auditdialog.cpp:5053`); results don't carry it. Ignore
   `runs[1..]` (current emit is single-run; INV-1 sets the
   canonical contract).
4. Walk `runs[0].results[]`. For each:
   - bump `count<Level>` from `level` field (default → `"warning"`
     when key absent; treated identically to explicit `warning` for
     both counts and sort).
   - if `suppressions[]` non-empty, also bump `countSuppressed`.
   - resolve `severity` via `ruleIndex[result.ruleId]`. If the rule
     id isn't in the index (foreign / older SARIF), fall back to
     mapping from `level`: `error → "CRITICAL"`, `warning → "MAJOR"`,
     `note → "INFO"`. (Foreign SARIF cannot surface `BLOCKER` or
     `MINOR` — those land only when the rule index resolves the
     value. Documented surface, not an error.)
   - if `level >= levelFloor` (per § 2.0 ordinal table), build a
     `Finding` for the topN candidate pool (full pool, not capped
     yet — sort needs the whole set).
5. Sort the candidate pool: level desc → confidence desc → file asc
   → line asc.
6. `resize(min(topN, pool.size()))`.
7. Set `runAtIso = runs[0].invocations[0].startTimeUtc` (empty if
   absent).
8. Stamp `sarifPath` from caller. Derive `htmlPath` in two passes:
   (a) replace the SARIF's extension with `.html`; if `QFile::exists`,
   use it. (b) Otherwise, fall back to the
   lexicographically-largest `audit-*.html` in the same
   `.audit_cache` dir **whose embedded timestamp is within ±60 s of
   the SARIF's** — the SARIF and HTML buttons each call
   `QDateTime::currentDateTime()` independently
   (`auditdialog.cpp:3344-3349` vs `:3375-3380`), so a same-session
   pair typically has a few seconds of skew but rarely more than a
   minute. The ±60 s bound prevents silent return of a stale
   90-day-old report when no current sibling exists. If neither
   match exists, leave empty.
9. Return `AuditSummary`.

### 3.2 New: `RemoteControl::cmdLastAuditSummary`

```cpp
// remotecontrol.h — public additions
QJsonDocument cmdLastAuditSummary(const QJsonObject &req);

// private additions — single-entry mtime cache
mutable QString  m_auditSummaryPath;     // last selected .sarif path
mutable qint64   m_auditSummaryMtimeMs = 0;
mutable AuditEngine::AuditSummary m_auditSummaryCache;
mutable int      m_auditSummaryCachedTopN = -1;
mutable QString  m_auditSummaryCachedFloor;
```

Body (sketch):

1. Validate `top_n` (default 5, clamp `[0, 50]`).
2. Validate `severity_floor` (default `"warning"`); on miss →
   `bad_severity_floor`.
3. Discover latest SARIF: scan
   `resolveRootCanonical(m_main) + "/.audit_cache"` for
   `audit-*.sarif`, pick the **lexicographically-largest filename**
   (timestamps are sortable). `audit-YYYYMMDD-HHmmss.sarif` →
   byte-string sort = chronological. Empty dir / no matches →
   `not_audited`. `resolveRootCanonical()` is the existing helper
   (`remotecontrol.cpp:1038`) used by ANTS-1248/1249/1250/1251 — same
   per-tab `shellCwd()` semantics.
4. mtime cache hit: if `path == m_auditSummaryPath` AND mtime ==
   cached AND `topN == m_auditSummaryCachedTopN` AND `floor ==
   m_auditSummaryCachedFloor` → build the wire envelope from
   `m_auditSummaryCache` (the parsed `AuditSummary` is what's
   cached; the `QJsonDocument` is rebuilt each hit since serialise
   is microseconds). **V1 caches the whole materialised summary
   keyed on the 4-tuple** — simplest implementation. Future split
   (parse-cache that survives topN/floor changes + summary-cache
   derived on demand) is non-breaking; defer until profiling shows
   topN/floor toggling is a real workload.
5. Cache miss: call `AuditEngine::summariseSarif(path, topN,
   floor)`; on `nullopt` → `parse_failed`.
6. Update cache members; build response envelope; return.

`resolveRootCanonical(MainWindow*)` already exists in
`remotecontrol.cpp:1038` and is used by every sibling MCP read-tool
(ANTS-1248/1249/1250/1251). It calls
`m_main->currentTerminal()->shellCwd()` so the tool naturally inherits
the focused-tab gate.

### 3.3 Wiring (post-ANTS-1253 surface)

- `claudeintegration.cpp` — one new entry in the `tools/list`
  schema builder (~12 LoC: name, description, inputSchema). **No
  new dispatch case** — registry handles it.
- `mainwindow.cpp` — one new `registerToolProvider("last_audit_summary",
  lambda)` call in `setupClaudeMcpProviders` (~10 LoC).

The lambda shape mirrors siblings (1247-1251):

```cpp
m_claudeIntegration->registerToolProvider("last_audit_summary",
    [this](const QJsonObject &args) -> QString {
        if (!m_remoteControl) return QString::fromUtf8(kRcUnavailable);
        return QString::fromUtf8(
            m_remoteControl->cmdLastAuditSummary(args)
                .toJson(QJsonDocument::Compact));
    });
```

`kRcUnavailable` is the post-1253 shared string literal.

## 4. Invariants

- **INV-1.** Reads SARIF only — never the HTML. HTML is render-only;
  SARIF is canonical (auditdialog emits both, the HTML is a
  derivative of the same in-memory data).
- **INV-2.** SARIF parse cached keyed on `(path, mtime, topN,
  severity_floor)` — same pattern shape as ANTS-1251. mtime drives
  invalidation: SARIF files are write-once *except* for the
  same-second collision path (see INV-6) where the second
  `QSaveFile::commit()` rename-overs the first. mtime updates on
  rename, so the cache invalidates correctly. `topN`/`floor`
  participate in the key because they affect the materialised
  summary.
- **INV-3.** `top_findings[]` sorted: SARIF level desc (per § 2.0
  ordinals; absent `level` is treated as `"warning"` for both sort
  and counts) → `properties.confidence` desc (-1 sorts last) →
  `artifactLocation.uri` asc (lexicographic) → `region.startLine`
  asc.
- **INV-4.** `counts` always reflects the *full* finding set, not
  the filtered `top_findings[]`. `countSuppressed` counts findings
  with non-empty `suppressions[]`; level counts include them.
- **INV-5.** Reachability gate inherits from UDS + MCP socket; no
  new auth surface. SARIF files are mode 0600 — readable only by
  the running user, which is also the only user who can reach the
  socket.
- **INV-6.** Latest-SARIF discovery uses **filename
  lexicographic-max**, not mtime. Filename embeds
  `YYYYMMDD-HHmmss` so byte-sort = chronological. mtime is unsafe
  (tar restore / `touch -d` / clock skew). **Same-second exports
  collide on disk** because `auditdialog.cpp:3347-3349` builds the
  timestamp at second granularity from `QDateTime::currentDateTime()`
  and writes via `QSaveFile` (atomic rename) — the second export
  silently overwrites the first. The tool surfaces whatever ends up
  at the lexicographic max; the cache (INV-2, mtime-keyed)
  invalidates correctly because the second write resets mtime.
- **INV-7.** `top_findings[].file` is whatever SARIF carries in
  `artifactLocation.uri` (typically project-relative; auditdialog
  does not absolutise — `auditdialog.cpp:5071` writes `f.file`
  as-is). The tool does **not** rewrite paths — caller resolves
  against `cwd`.
- **INV-8.** `severity_floor` validation runs **before** disk
  scanning. Saves a `readdir` on the bad-input path. Comparison is
  byte-exact lowercase against `{"error","warning","note"}` —
  `"Warning"` / `"ERROR"` / `"  warning"` all yield
  `bad_severity_floor`. (No normalisation; surface is small and the
  caller controls the spelling.)
- **INV-9.** `counts` is fixed-shape — always present, all four
  keys, even when zero. Saves caller a key-existence check.
- **INV-10.** Empty `runs[]` (SARIF skeleton with no actual run) →
  `not_audited` (treated identically to no SARIF on disk; the user
  has nothing to consult). Documented behaviour, not an error.
- **INV-11** (ANTS-3372). Parse-artifact guard. Before the INV-3
  sort+clamp, every parser (`summariseSarif` and the three foreign
  formats) drops findings that look like leaked scanner chrome — a
  progress-bar line scraped into the findings stream (RetroDB saw
  `{file:"Working... ━━ 100% 0", line:0, confidence:-1}` ranked as
  `top_findings[0]`). The predicate is conservative: it requires BOTH
  the no-location signature (`line == 0` **and** the default
  `confidence == -1`) **and** a junk-`file` signal — a box-drawing
  glyph (U+2500..U+257F) or a bare `%` with no `/` path separator —
  so a genuine project-level finding at line 0 with a real path is
  never dropped. `counts` (INV-4) is computed during the walk, before
  the filter, so the artifact does not change any count — only its
  rank in `top_findings[]`.

## 5. Memory & perf

**Memory budget**

- One `AuditSummary` cached per `RemoteControl` instance.
- `topFindings` capped at 50 × ~250B per Finding-summary
  (`ruleId`/`file`/`message`/etc.) ≤ 12.5 KiB.
- Path strings + counts + run_at: < 1 KiB.
- **Cache total: < 14 KiB steady-state.** No growth over the
  session lifetime — single-entry replacement on cache miss.

**Per-call latency** (assumes a local filesystem `.audit_cache` —
NFS / sshfs paths inflate every step in this list).

- Cache hit: ≤ 1 ms (string compares + mtime stat + JSON build).
- Cache miss: ≤ 50 ms for a 500 KiB SARIF (one `read()`, one
  `QJsonDocument::fromJson`, one `std::sort` over ≤ 2000 findings).
- Discovery (scan `.audit_cache` for `audit-*.sarif`): ≤ 10 ms even
  with hundreds of historical files (filename glob, no parse).

**Build cost (TU recompile fan-out)**

| File | Why it recompiles |
|------|-------------------|
| `src/auditengine.h` | New struct + new fn decl |
| `src/auditengine.cpp` | New fn body |
| `src/auditdialog.h` | Includes `auditengine.h` (header-level fan-out — every TU including this header recompiles transitively). |
| `src/auditdialog.cpp` | Includes `auditdialog.h`. |
| `src/settingsdialog.cpp` | Direct `#include "auditengine.h"`. |
| `src/remotecontrol.h` | New `cmdLastAuditSummary` decl + cache members; `mutable AuditEngine::AuditSummary m_auditSummaryCache` requires `#include "auditengine.h"` here (forward-decl insufficient — value member). |
| `src/remotecontrol.cpp` | New body. |
| `src/main.cpp` | Transitive: includes `remotecontrol.h` → also picks up `auditengine.h`. |
| `src/claudeintegration.cpp` | One new `tools/list` entry. |
| `src/mainwindow.cpp` | Includes `auditdialog.h` (transitive `auditengine.h`) and `remotecontrol.h`; plus the new `registerToolProvider("last_audit_summary", …)` call. |
| Each test bundle that links `auditengine` | re-link. |

Note (the table is bounded). `grep -rn '#include "auditengine.h"'
src/` returns 3 direct includers as of 2026-05-13 (`auditengine.cpp`,
`settingsdialog.cpp`, `auditdialog.h`); the table accounts for those
plus the second-order fan-out via `auditdialog.h`.

## 6. Token math

- **Per consultation today** (Read of HTML report): ~5-15 K tokens.
- **Per consultation with this tool** (default `top_n=5`): ~250
  tokens response. Schema cost is amortised across the session
  (paid once at `tools/list`), not per call.
- **Saving per fire: ~4.75-14.75 K tokens.**
- **Frequency:** ~1-3 audit consultations per session that touches
  audit findings; cumulative ~10-40 K tokens / such session.
- **Permanent schema cost:** ~80 tokens (single tool, two args) —
  paid once at `tools/list` regardless of how many calls follow.

## 7. Files touched

| File | Change | Est. LoC |
|------|--------|---------:|
| `src/auditengine.h` | `struct AuditSummary` + `summariseSarif` decl | ~25 |
| `src/auditengine.cpp` | `summariseSarif` body + sort-key helper | ~80 |
| `src/remotecontrol.h` | 1 public method + 5 cache members | ~10 |
| `src/remotecontrol.cpp` | `cmdLastAuditSummary` body + helpers | ~70 |
| `src/claudeintegration.cpp` | 1 `tools/list` entry (no new dispatch — 1253 registry) | ~12 |
| `src/mainwindow.cpp` | 1 `registerToolProvider` lambda | ~10 |
| `tests/features/mcp_last_audit_summary/` (new) | spec.md + test (10 INVs) | ~190 |
| `CMakeLists.txt` | Wire test bundle entry | ~1 |
| `ROADMAP.md` / `CHANGELOG.md` | ✅ + Changed entry | ~25 |
| **Total** | | **~423 LoC** |

**Net delta vs. stub estimate (~280 LoC):** spec full draft is ~50 %
larger than stub for two reasons — (a) `AuditSummary` struct fields
are explicit (the stub elided them), (b) cache key is 4-tuple not
just mtime (parse-vs-summarise distinction). Both are spec-cost only;
runtime cost is unchanged.

## 8. Risks / mitigations

| Risk | Mitigation |
|------|-----------|
| SARIF emit format drifts (auditdialog refactor) | Feature test asserts on a **fixture SARIF** (committed) — drift in the emit shape flips a separate red, not this tool's silent. |
| `.audit_cache` dir grows unbounded over months | Out of scope here; surface in `/debt-sweep` or in auditdialog (rotate-by-count). This tool only reads. |
| Stale cache survives a re-run with same filename | Cache is keyed on mtime (INV-2); a second same-second write resets mtime, invalidating the cache (INV-6). |
| User runs audit on project A, queries from project B | `resolveRootCanonical(m_main)` reads the focused tab's `shellCwd()` — same per-tab gate as ANTS-1251; no cross-tab leak. |
| SARIF file mode != 0600 (older audit run) | INV-5 narrative still holds (UDS gate is the real fence); tool reads whatever the FS hands it. |
| Empty `runs[]` SARIF | INV-10: `not_audited`. |
| Caller passes negative `top_n` | Clamp to 0 (returns empty `top_findings[]`, `counts` still populated — useful for "are there any?" probes). |
| Confidence absent (or unenriched) on a finding | The SARIF `properties.confidence` key is always present in current emit (`auditdialog.cpp:5099`), but may carry the in-memory default `-1` (`auditengine.h:98`) when the finding wasn't enriched. Spec response also reports `-1` if a foreign / older SARIF lacks the key entirely. Sort puts `-1` last either way. |

## 9. Out of scope (V1)

- **Audit trend graphs.** Auditdialog already tracks trend data;
  exposing it via MCP would be a separate verb. Defer until asked.
- **SARIF v2.2.** V1 reads v2.1.0 only (the only shape auditdialog
  emits). If 2.2 is added later, parser bumps the schema check.
- **Per-rule severity overrides.** SARIF carries
  `properties.severity` (the 5-level internal taxonomy) on each
  **rule** (`auditdialog.cpp:5050`); the tool resolves it per
  finding via the rule index built in § 3.1 step 3. Callers that
  want the resolved value sort/filter on `top_findings[].severity`.
  No new arg here.
- **Multi-run history** (last N runs, not just latest). Defer —
  current ask is "what's broken now"; "what changed across runs" is
  trend territory (above).
- **Pull a SARIF by absolute path arg.** V1 always picks latest;
  caller has the path back in the response if they need the file
  itself. Adding a `path` arg later is non-breaking.

## 10. step → verify plan

1. **Confirm landing order** — ANTS-1253 must be in (`registerToolProvider`
   surface) AND populated. Done as of commit `505f07e`.
   → verify: `grep -c 'registerToolProvider("' src/mainwindow.cpp`
   ≥ 12 (sibling tools registered). The header-only decl grep was
   too loose — moving the registry internals back wouldn't flip it.
2. `AuditEngine::summariseSarif` + struct.
   → verify: `grep -n 'summariseSarif' src/auditengine.h` = 1 decl,
   `src/auditengine.cpp` = 1 body.
3. `RemoteControl::cmdLastAuditSummary` + private cache members.
   → verify: `grep -cE 'mutable[^;]*m_auditSummary'
   src/remotecontrol.h` = 5 (restricts to the field decls; loose
   substring grep would drift if a header doc references the names).
4. `claudeintegration.cpp`: 1 new `tools/list` entry, **no** new
   `tools/call` dispatch case (registry handles it).
   → verify: `grep -c 'last_audit_summary' src/claudeintegration.cpp`
   = 1 (the schema entry only).
5. `mainwindow.cpp`: 1 `registerToolProvider("last_audit_summary",
   …)` call.
   → verify: `grep -c 'registerToolProvider("last_audit_summary"'
   src/mainwindow.cpp` = 1.
6. Write `tests/features/mcp_last_audit_summary/` (spec.md + test).
   → verify: pre-fix red — `git stash push -- src/` then re-run
   harness, expect ≥ 5/10 INVs to fail; `git stash pop`. (INV-5
   reachability, INV-7 path passthrough, INV-9 fixed-shape counts,
   INV-10 empty-runs are all shape contracts on the surface — they
   don't flip when the tool is absent because the harness never
   reaches the assertion.)
7. ctest green.
   → verify: `ctest --test-dir build --output-on-failure -R
   mcp_last_audit_summary` exits 0.
8. ROADMAP `📋 [ANTS-1254]` → `✅ [ANTS-1254]` with ship line.
   CHANGELOG `### Added` under `[Unreleased]`.
   → verify: `git diff ROADMAP.md CHANGELOG.md` shows both edits.

## 11. Cold-eyes loops (audit trail)

| Loop | Verified findings → fixes applied |
|----:|-----------------------------------|
| 1 | 1 critical (`projectRoot()` doesn't exist → `resolveRootCanonical(m_main)`) + 2 high (`parse_failed` family annotation; INV-6 same-second collision wording) + 4 medium (ordinal table § 2.0; example missing severity; build matrix transitives; confidence -1 phrasing). |
| 2 | 1 high (build matrix listed wrong includers — fixed to `auditengine.cpp`/`settingsdialog.cpp`/`auditdialog.h`) + 2 medium (INV count 8→10; confidence -1 reworded). |
| 3 | 3 high (severity field source via rule lookup; severity casing UPPERCASE per `severityLabel`; `html_path` lex-max fallback for separate SARIF/HTML buttons) + 3 medium (INV-3 absent-level; runs[0..] passthrough; local-FS assumption). |
| 4 | 1 high (step-1 verify tightened to `mainwindow.cpp` registry-call count ≥ 12) + 3 medium (INV-2 vs INV-6 reconciled; `m_auditSummary` grep restricted to mutable decls; html_path fallback bounded ±60 s). |
| 5 | 2 high (§ 9 OoS row 3 contradicted § 3.1 step 3 — fixed; `Finding` struct lacked `level`/`severity` strings — split to new `AuditSummaryFinding` struct) + 3 medium (`severityLabel` is static — engine carries own copy; `severity_floor` case-sensitivity documented; "emit cached envelope" wording clarified) + 3 low (token math double-count; foreign-SARIF can't surface BLOCKER/MINOR; step-6 INV count adjusted to ≥ 5/10). |

Stopped at loop 5 (the cold-eyes default cap). Loop-5 fixes
addressed the last two implementer-blockers (struct gap + spec
contradiction); subsequent loops would target precision-level
improvements that don't gate the implementer.
