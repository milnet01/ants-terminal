# ANTS-1295 — Per-tool cwd-anchor enforcement on path-accepting MCP tools

**Status:** spec draft → review → implement
**Kind:** security
**Source:** indie-review-2026-05-13
**Pairs with:** ANTS-1294 (MCP output sanitisation — same defense
layer, different attack surface) and the existing ANTS-1250
`validatePath` helper that already protects `git_state`.

---

## 1. Summary

Every MCP tool that accepts a filesystem path must anchor that path
to the current project root before any read, walk, exec, or write.
A path that resolves outside root → uniform rejection envelope
`{ok:false, error:"<tool>: \"<param>\" escapes project root",
code:"bad_path"}`. A single shared helper in
`src/pathvalidation.{h,cpp}` is the only place the anchor logic
lives; every path-accepting handler calls it.

## 2. Problem statement (threat model)

The MCP server runs in-process inside Ants Terminal and exposes ~30
tools to an in-tab Claude Code session. An attacker who can choose
the value of a `path`-typed parameter — by prompt-injecting the
assistant or by directly speaking JSON-RPC to the UDS — must not be
able to escape the focused tab's project root.

Concrete realisations:

- `file_outline path="/etc/passwd"` → reads `/etc/passwd`.
- `workspace_search lane="../../.."` → ripgreps the whole homedir.
- `debt_sweep_apply_fix file="../../tmp/x"` → calls `QFile::open`
  on a path outside root (read + potentially write).
- `indie_review_corroborate reports_dir="../../home/user"` → walks
  external `*.md` files.
- `git_state op=log path="../etc/passwd"` → runs `git log` with an
  out-of-root pathspec.

These five are the concrete vectors flagged by indie-review-2026-05-13.
The MCP `tools/call` registry at `src/mainwindow.cpp` has 31
`registerToolProvider` lines; the path-accepting subset is a
fraction of those, but the threat surface is one bad path away.

## 3. Surface — current state

Source survey (2026-05-14):

| MCP tool | Handler | Path param | Current anchor |
|---|---|---|---|
| `git_state` (log/diff) | `cmdGitState` → `runLogOp`/`runDiffOp` (`src/remotecontrol_state.cpp`) | `path` | ✅ Calls `validatePath()` |
| `file_outline` | `cmdFileOutline` (`src/remotecontrol_workspace.cpp`) | `path` | ⚠ Duplicates `validatePath` inline; an ANTS-1253 comment there explicitly says "will consolidate these into a shared helper" |
| `workspace_search` | `cmdWorkspaceSearch` (`src/remotecontrol_workspace.cpp`) | `lane` | ⚠ Duplicates `validatePath` inline + glob hygiene; emits `bad_lane` instead of `bad_path` |
| `indie_review_corroborate` | `cmdIndieReviewCorroborate` (`src/remotecontrol_review.cpp`) | `reports_dir` | ⚠ Engine `corroboratedFindingsFromDir` anchors silently (returns `{}` on escape); MCP layer does no validation |
| `cold_eyes_cross_doc_diff` | `cmdColdEyesCrossDocDiff` (`src/remotecontrol_coldeyes.cpp`) | `reports_dir` | ⚠ Same as above — silent `{}` from engine |
| `debt_sweep_apply_fix` | `cmdDebtSweepApplyFix` (`src/remotecontrol_review.cpp`) | `file` | ❌ **No anchor check anywhere.** Engine `applyMechanicalFix` does `projectPath + "/" + finding.file` and opens the file. A `Q_UNUSED(x)` marker on the resolved line gates the write, but the **read** is unconditional → information disclosure (file existence, marker presence). |
| `subsystem` | `cmdSubsystem` (`src/remotecontrol_state.cpp`) | `lane` (parsed-set member) | ✅ Lane validated against parsed CLAUDE.md map; files derived from a known-safe table |
| `indie_review_brief`, `cold_eyes_brief` | (lane only) | `lane` | ✅ Parsed-set lookup, no FS access |
| 22 other tools | — | none | n/a |

Three tools already use `validatePath` directly (the `git_state`
log/diff path). Three tools duplicate or under-validate. One
(`debt_sweep_apply_fix`) is unguarded at the MCP layer entirely.

## 4. Design

**One module, one helper, one call-site per handler.**

### 4.1 New module — `src/pathvalidation.{h,cpp}`

```cpp
// src/pathvalidation.h
#ifndef ANTS_PATHVALIDATION_H
#define ANTS_PATHVALIDATION_H

#include <QJsonObject>
#include <QString>

namespace PathValidation {

// Result of a single validation call.
struct Check {
    bool        bad = false;   // true → reject; err contains envelope
    QJsonObject err;           // {ok:false, error, code:"bad_path"}
    QString     argvForm;      // safe-for-argv form of accepted path
};

// Validate a user-supplied path against a canonicalised project root.
//
// Semantics:
//   rawPath empty       → Check{bad=false, argvForm=""} (caller decides
//                         if missing path is fatal).
//   Reject reasons      → bad=true with `err` populated:
//     "contains control or backslash characters"
//     "escapes project root"
//   On accept           → bad=false; argvForm == rawPath (NFC'd) with
//                         a `./` prefix added when it begins with `-`
//                         so it can't be misread as a CLI flag.
//
// Anchor logic mirrors the ANTS-1250 reference:
//   - NFC normalise rawPath.
//   - Reject control chars (U+0000..U+001F) or backslash.
//   - Absolute path used as-is; relative path joined to rootCanonical.
//   - If canonicalisable, the canonical form must equal rootCanonical
//     or start with rootCanonical + '/'.
//   - If not canonicalisable (path doesn't exist yet), apply the same
//     check to QDir::cleanPath(joined) so non-existent escapes still
//     reject.
//   - ANTS-1837: BOTH sides of that anchor comparison are NFC-normalised
//     (the shared `anchoredUnder` helper) before comparing. Without it a
//     decomposed (NFD) accented project root false-rejects a composed
//     (NFC) candidate that genuinely lives inside it, because rawPath is
//     NFC'd but the root previously was not. ASCII paths are unaffected.
//     The same NFC-aware anchor is exposed as
//     `PathValidation::isInsideProject(projectPath, candidate)` — a
//     boolean check for disk-derived paths (override configs) that never
//     pass through an MCP arg (used by coldeyesengine + indiereviewengine).
Check validatePath(const QString &rawPath,
                   const QString &rootCanonical,
                   const QString &toolName,
                   const QString &paramName = QStringLiteral("path"));

}  // namespace PathValidation

#endif
```

**Why a namespace + `Check`/`validatePath`** (and not a free
function in remotecontrol.h): the helper is self-contained, has no
dependency on `MainWindow`, and belongs in its own translation
unit so the future audit/ helper/ MCP-stub binaries can link it
without dragging remotecontrol's surface in. Mirrors the
`auditengine` split in CLAUDE.md.

**Why keep `resolveRootCanonical` in remotecontrol.cpp:** it depends
on `MainWindow::currentTerminal()`; the new module stays MainWindow-
free.

### 4.2 Wiring per handler

For each path-accepting handler:

```cpp
const QString root = resolveRootCanonical(m_main);
if (root.isEmpty()) return /* existing no_project envelope */;

const auto check = PathValidation::validatePath(
    rawPath, root, /*toolName*/ "<tool>", /*paramName*/ "<field>");
if (check.bad) return QJsonDocument(check.err);

// Use check.argvForm wherever the path flows into git argv;
// use the canonicalised resolved-form via QFileInfo for FS calls.
```

The five handlers that change:

1. **`cmdFileOutline`** — replace inline block (lines 1123–1156) with
   the `validatePath` call. Keep `not_found` (separate code) for the
   "path resolved but doesn't exist" case after canonicalisation.
2. **`cmdWorkspaceSearch`** — replace inline lane block (lines 905–920).
   Code change: `bad_lane` → `bad_path` for anchor reject. See § 8.
3. **`cmdIndieReviewCorroborate`** — add `validatePath` call before the
   `corroboratedFindingsFromDir(root, reportsDir, …)` invocation. The
   engine retains its own anchor as defense-in-depth (now redundant
   but harmless; future engines should rely on the MCP layer).
4. **`cmdColdEyesCrossDocDiff`** — same as above, for `reports_dir`.
5. **`cmdDebtSweepApplyFix`** — add `validatePath` for the `file`
   field before `DebtSweepEngine::applyMechanicalFix(root, f)`. This
   closes the unguarded read/write vector.

`cmdGitState` log/diff continue to use `validatePath`; the call sites
are updated only to pass the new `toolName`/`paramName` arguments.

### 4.3 Single chokepoint argument

The existing helper has been hardened over multiple iterations
(ANTS-1250 INV-5 added `./-` prefix; INV-8 added control-char
rejection). Centralising means future hardening lands once and
reaches all six callers. Distinct from ANTS-1294's outer wrap
(which sits at the dispatch site in `claudeintegration.cpp`),
this helper sits inside each handler — because the dispatch site
doesn't yet have access to the parsed `path` field. INV-1294
neutralises hostile *content*; INV-1295 neutralises hostile
*paths*.

## 5. Invariants

- **INV-1 — One helper.** `PathValidation::validatePath` in
  `src/pathvalidation.cpp` is the only place the
  canonicalFilePath + startsWith anchor logic exists. No inline
  duplicates remain in `remotecontrol.cpp`.

- **INV-2 — Six tools, eight call-sites.** Every path-accepting
  MCP tool calls `PathValidation::validatePath` exactly once per
  path-typed argument before any filesystem operation on that path.
  Six tools, eight call-sites (git_state has two paths flowing
  through `runLogOp` and `runDiffOp`; subsystem's
  `resolveLaneFiles` helper also routes through the validator as
  defence-in-depth against malicious symlinks inside src/):
  - `cmdGitState` (log + diff) — `runLogOp`/`runDiffOp`.
  - `cmdFileOutline` — replaces inline block.
  - `cmdWorkspaceSearch` — replaces inline block.
  - `cmdIndieReviewCorroborate` — new MCP-layer call.
  - `cmdColdEyesCrossDocDiff` — new MCP-layer call.
  - `cmdDebtSweepApplyFix` — new MCP-layer call.
  - `resolveLaneFiles` (subsystem belt-and-braces) — folded in
    while we were here; the lane itself is parsed-set-validated, but
    per-file symlink-escape rejection now goes through the helper
    too. Treated as a filter (skip on bad rather than emit envelope).

- **INV-3 — Uniform rejection envelope.** Every rejection emits
  `{"ok":false, "error":"<tool>: \"<param>\" <reason>",
  "code":"bad_path"}`. No tool uses `bad_lane` or any other code
  for path-anchor failures. (workspace_search retains
  `bad_pattern` for its independent pattern check; that's not an
  anchor concern.)

- **INV-4 — Symlink-resolving anchor.** The helper uses
  `QFileInfo::canonicalFilePath()` (which resolves symlinks)
  rather than `QDir::cleanPath()` (lexical only) when the path
  exists. Symlink-trick escapes from inside the tree to outside
  are rejected.

- **INV-5 — Non-existent paths still anchored.** A path that
  doesn't exist (e.g. a `git log` filter for a deleted file)
  falls back to `QDir::cleanPath()` and applies the same
  prefix check. `../../etc/passwd` rejects even if no such file
  exists below root.

- **INV-6 — Control-char and backslash reject.** NFC-normalised
  input must contain no U+0000..U+001F or `\\`. Closes nul-byte
  injection and Windows-path-confusion vectors.

- **INV-7 — argv-flag defense.** Accepted paths that begin with
  `-` are returned in `argvForm` with a `./` prefix so callers
  passing the value as a positional `git` arg cannot have it
  misread as a flag. (Inherits ANTS-1250-INV-5.)

- **INV-8 — Empty path is neutral.** An empty `rawPath` returns
  `Check{bad=false, argvForm=""}`. Callers that require a path
  check `rawPath.isEmpty()` themselves and emit their own
  "missing path" error (`bad_path` for `file_outline`, etc.).
  This keeps `git_state` semantics intact (no path filter when
  path absent).

- **INV-9 — Empty rootCanonical is reject.** If
  `resolveRootCanonical` returned empty (no focused project),
  every caller short-circuits before calling `validatePath`.
  The helper itself does not need to defend against empty root
  — the call-site contract handles it (existing pattern).

- **INV-10 — No state, no allocation past O(path).** The helper
  is a pure function. No caches, no static state, no global
  side effects. Per-call cost dominated by two `QFileInfo`
  constructions and one or two NFC normalisations.

## 6. Failure modes & rejection table

| Reject reason | `error` text | `code` | Trigger |
|---|---|---|---|
| Control char in path | `<tool>: "<param>" contains control or backslash characters` | `bad_path` | U+00..U+1F or `\\` in NFC-normalised input |
| Escapes project root (existing) | `<tool>: "<param>" escapes project root` | `bad_path` | canonical path is outside root |
| Escapes project root (non-existent) | same | `bad_path` | cleaned join is outside root |

Per-handler missing-path errors (`"missing or empty \"path\""`
etc.) are emitted by the caller before invoking validatePath;
spec-out-of-scope for the helper itself.

## 7. Test plan

**`tests/features/mcp_path_anchor/`** (new).

### Engine tests — call `PathValidation::validatePath` directly

- **PV-1 empty rawPath → bad=false, empty argvForm.**
- **PV-2 simple relative inside root** (`src/main.cpp`) → bad=false,
  argvForm == `src/main.cpp`.
- **PV-3 dash-prefixed accept** (`-rm-rf`) → bad=false, argvForm
  == `./-rm-rf`.
- **PV-4 relative traversal** (`../../etc/passwd`) → bad=true,
  err.code == `bad_path`, err.error contains tool name + param
  name + "escapes project root".
- **PV-5 absolute outside root** (`/etc/passwd`) → bad=true,
  same envelope.
- **PV-6 control character** (`a\x01b`) → bad=true, err.error
  contains "control or backslash".
- **PV-7 backslash** (`a\\b`) → bad=true, same.
- **PV-8 NFC normalisation** — composed vs decomposed form of the
  same path resolves identically.
- **PV-9 non-existent escape** (`nonexistent/../../etc/passwd`)
  → bad=true (lexical-cleanPath fallback rejects).
- **PV-10 non-existent inside-root accept** (`new/file/path`)
  → bad=false (lexical-cleanPath fallback accepts), argvForm
  carries through.

Engine tests use a real tmpdir as `rootCanonical` (created via
`QTemporaryDir`), so canonicalisation is meaningful.

### Wiring tests — source-grep

- **WI-1** `src/pathvalidation.h` exists and exposes
  `PathValidation::validatePath`.
- **WI-2** `src/pathvalidation.cpp` defines it and contains the
  canonicalFilePath + startsWith pair.
- **WI-3** `remotecontrol.cpp` no longer contains a
  `canonicalFilePath()` + `startsWith(rootCanonical` anchor pair —
  the anchor logic lives only in `src/pathvalidation.cpp`. Two
  `startsWith(rootCanonical` lines remain at `remotecontrol.cpp`
  but they're response-path **reframing** (strip root prefix from
  result paths so callers see project-relative paths), not anchor
  checks. WI-3 pins the count: ≤ 2 occurrences of
  `startsWith(rootCanonical` in `remotecontrol.cpp`, and zero of
  them are preceded within 5 lines by a `canonicalFilePath()`
  return-value assignment.
- **WI-4** Eight `PathValidation::validatePath(` call-sites in
  `remotecontrol.cpp`: `cmdWorkspaceSearch`, `cmdFileOutline`,
  `runLogOp`, `runDiffOp`, `resolveLaneFiles`,
  `cmdIndieReviewCorroborate`, `cmdDebtSweepApplyFix`,
  `cmdColdEyesCrossDocDiff`.
- **WI-5** `bad_lane` no longer appears as an anchor-reject code in
  `cmdWorkspaceSearch` (the string `"bad_lane"` may persist in other
  contexts — e.g. unknown-lane errors — but not next to the
  validatePath result for the anchor step).

### Pre-fix regression check

Engine tests fail to link against pre-fix code (helper doesn't
exist). Wiring tests fail because the inline blocks are still in
remotecontrol.cpp. Both failure modes are loud.

## 8. Migration / back-compat

- **`bad_lane` (anchor) → `bad_path`.** `cmdWorkspaceSearch` previously
  returned `{code:"bad_lane"}` for path-escape and control-char
  rejections on the `lane` field. After ANTS-1295 these emit
  `{code:"bad_path"}`. This is a documented MCP-tool contract change.
  CHANGELOG entry under `### Security` notes the rename and the
  rationale. The independent `bad_pattern` and `bad_glob` codes are
  unaffected. Consumers checking specifically for `bad_lane` for the
  *lane-anchor* case must update; consumers checking only `ok ==
  false` are unaffected.

- **Engine-internal anchors retained.** `corroboratedFindingsFromDir`
  / `crossDocDiffFromDir` keep their existing anchor checks even
  though the MCP layer now validates first. This is intentional:
  these engines are also called from non-MCP code paths (CI runners,
  the helper CLI), and removing the internal check would degrade
  defense in depth.

## 9. Performance & RAM budget

- **Per-call:** 1 NFC normalise (O(len)), 2 `QFileInfo` constructions,
  1 `QDir::filePath()`, 1 `QFileInfo::canonicalFilePath()` syscall
  (`realpath`-equivalent). On a typical 50-byte project-relative path:
  ~3–10 µs.
- **Idle:** zero. No timers, no caches, no watchers, no static state.
- **RAM:** stack frame only; no heap allocations past the QString
  temporaries on input. ≪ 1 KB per call, returned at scope exit.
- **Wire overhead:** zero on accept (argvForm replaces the raw value
  in argv only). On reject: the small envelope adds ~120 bytes vs.
  silent `{}` engine returns; mostly a UX gain (callers see why).

Total new RAM/CPU budget: negligible. No long-lived structures
introduced.

## 10. Out of scope

- Sandboxing the engine writers (QSaveFile-level). Anchor at the
  MCP layer is the contract; the engine still calls `QFile::open`
  directly. A future ticket can sandbox via seccomp, but ANTS-1295
  is specifically about per-tool input validation.
- Extending validatePath to validate *globs* (workspace_search's
  separate `glob` field already has its own hygiene block — the
  256-byte cap + `..` substring reject). Keeping anchor and glob
  validation separate matches the existing source layout.
- Validating paths inside JSON arrays (e.g. `cold_eyes_fold_in
  actionable[].file`). Those handlers do not currently open any
  file at the named path — they write fold-in blocks to
  ROADMAP.md only — so they're not in the threat model. If a
  future handler reads from those paths, it must call validatePath.
- The roadmap text references `path_a`/`path_b`/`paths`/`dir`/`cwd`/
  `file`/`filename` as path-like parameter names. The current
  registry uses `path`, `lane`, `reports_dir`, `file`. None of the
  other names occur today; this spec covers exactly the four that
  do.

## 11. Cold-eyes review log

**Loop 0 (initial draft, 2026-05-14):** Drafted above.

**Loop 1 — VERIFY against current source (2026-05-14):**
- ✅ `validatePath` defined at `remotecontrol.cpp:1237`; called at
  1424 (`runLogOp`) and 1537 (`runDiffOp`).
- ⚠ Found stale line ranges:
  - `cmdFileOutline` inline block: claimed 1124–1154, actual
    **1123–1156**. ANTS-1253 comment at line **1107**, not 1124.
    → Fixed.
  - `cmdWorkspaceSearch` lane block: claimed 894–921, actual lane
    sub-block **905–920** (the wider root-check span is 894–920).
    `bad_lane` emit sites at **896, 909, 917**. → Fixed.
- ✅ `cmdIndieReviewCorroborate` at 2143; engine call to
  `corroboratedFindingsFromDir` at 2180 anchors (INV-3 at
  `indiereviewengine.cpp:430`) but returns `{}` silently on
  escape — confirmed.
- ✅ `cmdDebtSweepApplyFix` at 2414; engine `applyMechanicalFix`
  at `debtsweepengine.cpp:635` does `projectPath + '/' + finding.file`
  with **no anchor check**. Confirmed unguarded vector.
- ✅ `cmdColdEyesCrossDocDiff` at 2944; delegates to engine which
  re-delegates to `corroboratedFindingsFromDir` (same anchor).
- ✅ `cmdSubsystem` lane is a parsed-set member from
  `SubsystemMap::cachedLanes(CLAUDE.md)`; safe.
- ⚠ INV-2 / WI-4 had a "six vs seven" inconsistency (six tools,
  seven call-sites because git_state has two). → Fixed.

Status: spec consistent with current source after the four edits.

**Loop 2 — Internal consistency (2026-05-14):**
- INV-1..10 reviewed; no contradictions between them.
- Every INV mapped to at least one test case:
  - INV-1 ↔ WI-3 (no inline anchor pattern remains)
  - INV-2 ↔ WI-4 (call-site count)
  - INV-3 ↔ PV-4/5 (rejection envelope shape) + WI-5 (`bad_lane`
    retired)
  - INV-4 ↔ tested via real `QTemporaryDir` (symlink-resolving
    semantics inherent in `QFileInfo::canonicalFilePath`)
  - INV-5 ↔ PV-9 (non-existent escape rejects)
  - INV-6 ↔ PV-6/7 (control char + backslash)
  - INV-7 ↔ PV-3 (dash-prefix → `./` argvForm)
  - INV-8 ↔ PV-1 (empty rawPath neutral)
  - INV-9 ↔ design constraint, caller-enforced; not separately
    testable in the helper unit
  - INV-10 ↔ design constraint; perf measured by absence of new
    static state in the new TU
- Test plan covers every reject reason in the § 6 table.
- Migration § 8 lists the only contract-breaking change
  (`bad_lane` retirement) and gives the rationale.

Status: **CLEAN.** Ready to implement.

---

*End of spec.*
