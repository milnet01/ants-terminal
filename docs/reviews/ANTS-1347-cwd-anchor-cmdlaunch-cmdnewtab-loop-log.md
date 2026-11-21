# ANTS-1347 — `cwd` anchor on `cmdLaunch` / `cmdNewTab` via `PathValidation::validatePath` — review loop log

Record for [`../specs/ANTS-1347-cwd-anchor-cmdlaunch-cmdnewtab.md`](../specs/ANTS-1347-cwd-anchor-cmdlaunch-cmdnewtab.md) § 11. Cold-eyes review log.

## Cold-eyes loop log

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
