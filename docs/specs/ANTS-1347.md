# ANTS-1347 — `cwd` anchor on `cmdLaunch` / `cmdNewTab` via `PathValidation::validatePath`

**Status:** spec draft → review → implement
**Kind:** security (path-anchor consistency)
**Source:** indie-review-2026-05-14 (lane-2 M3)
**Pairs with:** ANTS-1295 (per-tool path-anchor enforcement — same
helper, eighth and ninth call-sites). Bundle-A sibling to ANTS-1335
(byte-level rc hardening).

---

## 1. Summary

`cmdLaunch` and `cmdNewTab` (`src/remotecontrol_terminal.cpp`)
accept a `cwd` field. Each currently runs a hand-rolled
`cwdHasControl` lambda (lines 513-517 and 564-568) that rejects only
control characters and backslash. Neither verb anchors the `cwd`
against the focused tab's project root — same-UID rc/MCP peers can
chdir a new tab into `/etc`, `/var/log`, `/proc`, or any reachable
directory.

Fix: route both fields through `PathValidation::validatePath`
(introduced by ANTS-1295) with an explicit "allow absolute path
outside root" flag, OR keep them unanchored and document the trust
assumption with a `[[reason]]`-style comment naming the threat model.

This spec proposes **anchor-by-default with an explicit
`allow_outside_root` opt-out** for the rare consumer that legitimately
needs to chdir outside the project (e.g., a Lua plugin opening a
support-bundle directory). Keeps the project-root-anchor
philosophy uniform across the rc surface while preserving the
documented "shell new-tab anywhere" workflow for callers that opt in.

## 2. Problem statement (threat model)

Trust model on the rc UDS socket (per ANTS-1132): same-UID +
`SO_PEERCRED` + 0700 perms. The bytes are trusted to be from a
process the user owns; the *content* of those bytes is not. The rc
seam is the boundary across which user-controlled or
prompt-injected JSON crosses into Ants-controlled execution.

Every other path-typed argument on the rc/MCP surface — `file_outline
path`, `workspace_search lane`, `git_state path`,
`debt_sweep_apply_fix file`, `indie_review_corroborate reports_dir`,
`cold_eyes_cross_doc_diff reports_dir` (post-ANTS-1295) — anchors
to the focused project root. `cmdLaunch.cwd` and `cmdNewTab.cwd`
are the outliers.

Concrete realisations of the unanchored vector:

- **Sensitive-directory chdir** — `{"cmd":"launch","cwd":"/etc",
  "command":"ls -la"}` opens a new tab in `/etc` and runs `ls -la`.
  The output reaches the PTY and is visible in the new tab's
  scrollback — readable by anyone with screen access to the same
  Ants instance.
- **`/proc/self` snooping** — `cwd="/proc/self/fd"` exposes the
  Ants process's open file descriptors to the launched command's
  cwd resolution.
- **`/var/log` tailing** — `cwd="/var/log",command="tail -F syslog"`
  creates a passive log-tail in a new tab; benign per se, but
  uniformly anchored elsewhere this is an inconsistency.
- **Symlink-escape** — even with the existing `cwdHasControl`
  rejecting `\\`, a path like `./local-symlink-to-root` that
  resolves outside the project goes through.

The threat is **not** "an attacker chrooted into Ants" — same-UID
already grants full chdir via `os.chdir()` in any other process.
The threat is **trust-boundary consistency**: a Claude Code session
or rc peer that's been audited for "doesn't reach outside the
project" relies on every rc verb maintaining that anchor.
ANTS-1295's premise — "anchor at the boundary, not the kernel" —
applies here too.

## 3. Surface — current state

| Verb | Site | Current cwd check |
|---|---|---|
| `cmdLaunch` | `src/remotecontrol_terminal.cpp` | inline `cwdHasControl`: rejects U+0000..U+001F + `\\` only |
| `cmdNewTab` | `src/remotecontrol_terminal.cpp` | inline `cwdHasControl`: same |
| `cmdGitState log path` | `src/remotecontrol_state.cpp` | ✅ `PathValidation::validatePath` |
| `cmdGitState diff path` | `src/remotecontrol_state.cpp` | ✅ `PathValidation::validatePath` |
| `cmdFileOutline path` | `src/remotecontrol_workspace.cpp` | ✅ post-ANTS-1295 |
| `cmdWorkspaceSearch lane` | `src/remotecontrol_workspace.cpp` | ✅ post-ANTS-1295 |
| `cmdSubsystem resolveLaneFiles` | `src/remotecontrol_state.cpp` | ✅ post-ANTS-1295 (belt-and-braces filter; lane itself parsed-set-validated) |
| `cmdDebtSweepApplyFix file` | `src/remotecontrol_review.cpp` | ✅ post-ANTS-1295 |
| `cmdIndieReviewCorroborate reports_dir` | `src/remotecontrol_review.cpp` | ✅ post-ANTS-1295 |
| `cmdColdEyesCrossDocDiff reports_dir` | `src/remotecontrol_coldeyes.cpp` | ✅ post-ANTS-1295 |

Eight existing `PathValidation::validatePath` call-sites
(`grep -n "PathValidation::validatePath" remotecontrol.cpp` returns
910, 1117, 1340, 1455, 1709, 2105, 2369, 2931). Two outliers,
one helper.

## 4. Design

### 4.1 New optional request field — `allow_outside_root`

```json
{"cmd":"launch","cwd":"<path>","command":"<string>",
 "allow_outside_root":<bool optional, default false>,
 "raw":<bool optional>}
```

(Same field added to `cmdNewTab`.)

- Default `false`: `cwd` MUST anchor to the focused project root.
  Behaves like every other path-typed rc verb.
- `true`: bypasses the anchor check; the existing
  `cwdHasControl` (NFC + control + backslash) still applies, plus
  the C1 strip from ANTS-1335 applies. Documented in the verb's
  header comment.
- Empty/absent `cwd` continues to mean "inherit cwd from the focused
  terminal" — both anchor and `allow_outside_root` are skipped on
  the empty-cwd path.

### 4.2 Wiring per verb

```cpp
// cmdLaunch — replace lines 507-526
const QString cwd = req.value("cwd").toString();
if (!cwd.isEmpty()) {
    const bool allowOutside =
        req.value("allow_outside_root").toBool(false);
    if (allowOutside) {
        // Opt-out path: keep the existing C0/backslash hygiene
        // (ANTS-1052), add C1 hygiene as a peer of the byte filter
        // in ANTS-1335.
        if (cwdHasBadByte(cwd)) {
            QJsonObject err;
            err["ok"] = false;
            err["error"] = QStringLiteral(
                "launch: cwd contains control or backslash characters");
            err["code"] = QStringLiteral("bad_cwd");
            return QJsonDocument(err);
        }
    } else {
        // Default path: anchor to the focused project root.
        const QString root = resolveRootCanonical(m_main);
        if (root.isEmpty()) {
            QJsonObject err;
            err["ok"] = false;
            err["error"] = QStringLiteral(
                "launch: no focused project root (set "
                "allow_outside_root:true to chdir outside any project)");
            err["code"] = QStringLiteral("no_project");
            return QJsonDocument(err);
        }
        const auto check = PathValidation::validatePath(
            cwd, root, QStringLiteral("launch"),
            QStringLiteral("cwd"));
        if (check.bad) return QJsonDocument(check.err);
    }
}
const int idx = m_main->newTabForRemote(cwd, filteredCommand);
```

`cmdNewTab` mirrors the change verbatim, swapping `"launch"` for
`"new-tab"` in the envelope strings.

`cwdHasBadByte` is the existing `cwdHasControl` lambda factored to
also reject the C1 UTF-8 sequence (`0xC2 [0x80..0x9F]`) at the
QString level (input is already QString-decoded; check on
`QChar::unicode()`). One per-verb lambda becomes a shared helper:

```cpp
// remotecontrol.cpp — file-scope static
static bool cwdHasBadByte(const QString &raw) {
    const QString nfc = raw.normalized(QString::NormalizationForm_C);
    for (QChar c : nfc) {
        const ushort u = c.unicode();
        if (u < 0x20) return true;             // C0
        if (u == 0x5C) return true;            // backslash
        if (u >= 0x80 && u <= 0x9F) return true;  // C1
    }
    return false;
}
```

### 4.3 Composition with ANTS-1335

ANTS-1335 strips C1 bytes from `text` / `command` payloads at the
rc filter. The `cwd` field never reaches the PTY — it goes through
`m_main->newTabForRemote(cwd, ...)`, where it ultimately becomes
`chdir`'s argument. Stripping C1 from a path silently would change
the path's identity; rejecting instead is the right move.

`cwdHasBadByte` is the path-side analogue of `filterControlChars`:
reject-not-strip semantics for fields where silent mutation would
mislead.

### 4.4 Error envelope (anchor reject)

When the anchor check rejects, the response uses ANTS-1295's
`bad_path` code for cross-verb consistency:

```json
{"ok":false,
 "error":"launch: \"cwd\" escapes project root",
 "code":"bad_path"}
```

The existing `bad_cwd` code is retained only for the
control/backslash/C1 reject (a different class of error, predates
the anchor check). Documented in § 8 migration.

## 5. Invariants

- **INV-1 — Anchor on by default.** Every `cmdLaunch`/`cmdNewTab`
  request with a non-empty `cwd` and `allow_outside_root != true`
  routes `cwd` through `PathValidation::validatePath` against
  `resolveRootCanonical(m_main)`.

- **INV-2 — Empty `cwd` is neutral.** Empty/absent `cwd` skips both
  the anchor and the opt-out path. The verb falls through to
  `newTabForRemote` which uses the focused terminal's cwd.

- **INV-3 — `allow_outside_root:true` skips anchor but not hygiene.**
  The opt-out bypasses anchor-to-root but the byte-hygiene check
  (`cwdHasBadByte`) still applies. No request path skips byte
  hygiene.

- **INV-4 — C1 rejected on `cwd`.** `cwdHasBadByte` rejects U+0080..
  U+009F. This is the path-side counterpart to ANTS-1335's
  byte-strip on payloads. Reject-not-strip is the right semantic
  for paths (silent mutation misleads).

- **INV-5 — Two verbs, one helper.** Both `cmdLaunch` and
  `cmdNewTab` call `cwdHasBadByte` and (default-path)
  `PathValidation::validatePath`. The two old per-verb
  `cwdHasControl` lambdas in `cmdLaunch` and `cmdNewTab` are removed;
  the shared helper replaces them.

- **INV-6 — Anchor reject emits `bad_path`.** Matches the
  ANTS-1295 cross-verb code. The pre-existing `bad_cwd` code is
  reserved for the byte-hygiene reject (different class).

- **INV-7 — No-focused-root + default-path is hard-reject.** If
  `resolveRootCanonical(m_main)` returns empty (no focused project),
  a request with `cwd` and `allow_outside_root != true` rejects with
  `code:"no_project"` — same envelope shape ANTS-1295 uses, with a
  message that names the `allow_outside_root` opt-out so the caller
  knows how to retry.

- **INV-8 — Opt-out is request-scoped.** `allow_outside_root` is
  per-request; no session-state or configuration toggle. Each
  request stands alone.

- **INV-9 — Existing same-UID trust model holds.** This spec adds a
  defence-in-depth check; it doesn't claim to defend against a
  hostile same-UID attacker beyond what `SO_PEERCRED` + 0700 perms
  already provide. The aim is consistency with the broader
  rc/MCP anchor philosophy.

- **INV-10 — Wire schema additive.** `allow_outside_root` is a new
  optional bool. No existing field is renamed, removed, or shifted.

## 6. Failure modes & rejection table

| Reject reason | `error` text | `code` |
|---|---|---|
| `cwd` byte-hygiene fail | `<verb>: cwd contains control or backslash characters` | `bad_cwd` (preserved) |
| `cwd` escapes project root | `<verb>: "cwd" escapes project root` | `bad_path` |
| `cwd` set, no focused project, opt-out absent | `<verb>: no focused project root (set allow_outside_root:true to chdir outside any project)` | `no_project` |

## 7. Test plan

**`tests/features/rc_launch_cwd_anchor/`** (new).

### Engine tests

- **LC-1 happy path inside root** — `cwd="docs/"`, project root set
  → accept, tab opens.
- **LC-2 empty cwd** — `cwd=""` → accept (inherits focused cwd).
- **LC-3 traversal reject default** — `cwd="../../etc/passwd"`,
  default `allow_outside_root` → reject with `bad_path`.
- **LC-4 absolute outside root reject default** — `cwd="/etc"` →
  reject with `bad_path`.
- **LC-5 opt-out absolute outside accept** —
  `cwd="/etc", allow_outside_root:true` → accept (bypass anchor,
  byte-hygiene passes).
- **LC-6 opt-out + bad byte reject** —
  `cwd="/etc\x01", allow_outside_root:true` → reject with
  `bad_cwd` (hygiene still applied).
- **LC-7 C1 in cwd reject** — `cwd="docs\xC2\x9Bsubdir"` (decoded
  to U+009B in QString) → reject with `bad_cwd`. New C1 path.
- **LC-8 no focused project default** — `resolveRootCanonical`
  empty, `cwd="something"`, default opt-out absent → reject with
  `no_project`; message mentions `allow_outside_root`.
- **LC-9 no focused project opt-out** — same as LC-8 with
  `allow_outside_root:true` → accept.
- **LC-10 same coverage for `new-tab`** — repeat LC-1, LC-3, LC-4,
  LC-5, LC-7 against `cmdNewTab`.

### Wiring tests — source-grep

- **WI-1** `remotecontrol.cpp` contains exactly one
  `static bool cwdHasBadByte(` definition.
- **WI-2** The two old `cwdHasControl = []` lambdas at
  `cmdLaunch` and `cmdNewTab` are removed (zero occurrences of
  the literal `cwdHasControl`).
- **WI-3** Each of `cmdLaunch` and `cmdNewTab` call
  `PathValidation::validatePath` exactly once. Total call-sites
  in the file rises from ANTS-1295's eight to **ten**.
- **WI-4** `"allow_outside_root"` appears in `cmdLaunch` and
  `cmdNewTab` bodies and nowhere else.

### Pre-fix regression check

LC-3, LC-4, LC-7, LC-8 all pass against pre-fix code (no anchor
check), so they fail-against-the-bug correctly only post-patch.
Pre-patch behaviour: `cwd="/etc"` accepts and the new tab opens in
`/etc`. The failing-pre-patch test asserts the post-patch reject
envelope.

## 8. Migration / back-compat

- **`bad_cwd` retained for byte-hygiene reject.** Existing callers
  that branch on `code == "bad_cwd"` continue working for the
  control/backslash class; new `bad_path` code distinguishes the
  anchor-escape class. Documented in CHANGELOG under "MCP".

- **`allow_outside_root` field default-false:** existing callers
  that omit the field now get an anchor check. Any plugin or
  script that has been silently opening `/etc`-style cwds breaks
  with a clear `no_project` or `bad_path` envelope; the fix is a
  one-line `"allow_outside_root":true` addition. This is the
  intended behaviour — the unanchored default was a gap, not a
  feature.

- **CHANGELOG entry** under "Security" notes the new default plus
  the opt-out, and links to ANTS-1295 for the helper.

## 9. Performance & RAM budget

- **Default path:** one `resolveRootCanonical(m_main)` (which
  already runs for every path-anchored verb; result is a QString
  literal copy, ~µs), one `PathValidation::validatePath` (~3-10 µs
  per ANTS-1295 § 9).
- **Opt-out path:** zero overhead beyond the existing byte-hygiene
  loop. Same as today.
- **No heap growth.** No statics, no caches.

Total new RAM/CPU budget: negligible. Inherits from ANTS-1295.

## 10. Out of scope

- Generalising the opt-out across other rc verbs. Other verbs
  (file_outline, workspace_search, git_state) have a *file-data*
  semantic where outside-root never makes sense. `cmdLaunch` and
  `cmdNewTab` are the only verbs with a legitimate "chdir
  anywhere" use case.
- Recording `allow_outside_root:true` usage in a session log. A
  future ticket might surface "this session opened N tabs outside
  any project root" as a security-audit signal. Out of scope here.
- Restricting `allow_outside_root` by Lua plugin manifest. The
  plugin layer already runs as the same UID; per-plugin
  restriction needs the broader plugin-permission framework
  (separate roadmap item).

## 11. Cold-eyes review log

**Loop 0 (initial draft, 2026-05-15):** Drafted above.

**Loop 1 — VERIFY against current source (2026-05-15):**

- ✅ `cmdLaunch` at `remotecontrol.cpp:466-532`; cwd hygiene block
  at `:507-526`. Inline `cwdHasControl` lambda at `:513-517`.
- ✅ `cmdNewTab` at `:534-583`; cwd hygiene block at `:551, 563-577`.
  Identical `cwdHasControl` lambda at `:564-568`.
- ⚠ Loop 0 § 3 cited line numbers from the ANTS-1295 spec time
  (`1424, 1537` for git_state; `1093` for file_outline; etc.).
  Current source shows the 8 `PathValidation::validatePath`
  call-sites are at lines **910, 1117, 1340, 1455, 1709, 2105,
  2369, 2931**. Surface table updated.
- ✅ `subsystem`'s `resolveLaneFiles` call-site at `:1709` is the
  eighth site (the one ANTS-1295 § 5 INV-2 called out as the
  belt-and-braces filter — comment at `:1705-1708` confirms).
  After this spec lands, total rises to **10** sites
  (`cmdLaunch` + `cmdNewTab` add two each? No — one each).
- ✅ WI-3 corrected: 8 + 2 = **10**. Spec § 7 WI-3 already says
  "Total call-sites in the file rises from ANTS-1295's eight to
  **ten**." Consistent with Loop 1 measurement.
- ✅ `PathValidation::Check` struct (`pathvalidation.h:24-29`)
  exposes `bad`, `err`, `argvForm`, `resolved` — the spec's patch
  sketch only uses `check.bad` and `check.err`, which matches
  the contract of "anchor-reject only; don't need canonical
  resolved path for cwd" (the cwd is passed verbatim to
  `newTabForRemote` which does its own chdir).
- ✅ `cwdHasBadByte` helper composes with ANTS-1335's
  `filterControlChars` for the `command` field — the two
  hardenings cover disjoint dimensions (path vs payload) and
  share the C1 codepoint range U+0080..U+009F.
- ✅ INV-1 through INV-10 reviewed. Every INV maps to at least
  one LC-* or WI-* test:
  - INV-1 ↔ LC-3, LC-4, LC-10
  - INV-2 ↔ LC-2
  - INV-3 ↔ LC-5, LC-6
  - INV-4 ↔ LC-7
  - INV-5 ↔ WI-1, WI-2
  - INV-6 ↔ LC-3 (verifies `code == "bad_path"`)
  - INV-7 ↔ LC-8
  - INV-8 ↔ design constraint; per-request scope inherent in patch
  - INV-9 ↔ same-UID trust model unchanged (no test needed)
  - INV-10 ↔ WI-4 (additive field only)
- ✅ Cross-doc: composes with ANTS-1335 (rc payload byte filter)
  and ANTS-1295 (path helper). The three Bundle-A specs share
  the rc-seam threat model and don't conflict.
- ✅ RAM budget § 9 unchanged — inherits ANTS-1295's
  validatePath cost (~3-10 µs per call).

Status: **CLEAN.** Ready for Loop 2 implementation pass.

---

*End of spec.*

---

*End of spec.*
