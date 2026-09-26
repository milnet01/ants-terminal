# ANTS-1337 — cold-eyes review log

Review history for [`docs/specs/ANTS-1337.md`](../specs/ANTS-1337.md), moved
out of its § 11 on 2026-09-26. Entries are unchanged.

## Cold-eyes loop log


**Loop 0 (initial draft, 2026-05-15):** Drafted above.

**Loop 1 — VERIFY against current source (2026-05-15):**

- ✅ `VerifyEngine::runOneGate` at `verifyengine.cpp:262-345`;
  the `/bin/sh -c` spawn is at `:291-292`.
- ✅ `loadGateConfig` at `:353-394`; reads
  `projectPath/.ants/verify.json`, anchors via
  `pathStrictlyBelow` at `:367` (ANTS-1289 INV-4), parses via
  `parseVerifyJson` at `:373`.
- ✅ `cmdVerifyChanges` MCP handler at `remotecontrol.cpp:2539`.
  Fixed § 3 surface table + WI-3 — Loop 0 had "TBD via Loop 1"
  placeholders.
- ⚠ Loop 0 § 4.6 + § 8 referenced `Config::saveAtomic` as the
  atomic-write helper. The codebase doesn't expose such a helper
  — `config.cpp:203` does the work inline with
  `std::rename(tmp, dst)`. Two existing helpers in `secureio.h`
  compose the missing pieces: `setOwnerOnlyPerms` at
  `secureio.h:33` (0600) and `fsyncParentDir` at `secureio.h:47`
  (ext4 durability post-rename, ANTS-1141 pattern). Rewrote § 4.6
  to reference these directly instead of inventing a new helper.
  § 8 migration note adjusted to match.
- ✅ Modal-on-main-thread design: `QLocalServer::newConnection`
  fires on the main thread and per-request handlers run there
  (per `secureio.h:58-71` trust-model comment). So a
  `QMessageBox::exec()` from within `cmdVerifyChanges` blocks
  only that one connection; the rest of the UI stays
  responsive. INV-8's `BlockingQueuedConnection` invocation
  pattern is forward-compatible for a future worker-thread rc
  but not required today.
- ✅ Session-cache (INV-9) prevents prompt-fatigue. The cache
  lives in RAM only — restarting Ants clears it, so a user who
  Denied once today still gets prompted tomorrow (matching
  user-expectation of "long-lived denials are explicit").
- ✅ Headless mode (INV-6): the test plan's VT-6 confirms the
  no-MainWindow path. CI runners that exercise `verify_changes`
  programmatically (via `ants-helper` v2 or a future MCP-stub
  binary) will see the auto-detect fallback and surface
  `verify_untrusted:true` — correct, non-silent.
- ✅ Sample-command surfacing in modal (§ 4.3): displays first
  200 chars of `gc.command`. Long commands truncate visibly so
  the user can hover to see the rest. Mitigates the "OK button
  fatigue" failure mode where users approve without reading.
- ✅ INV-1 through INV-10 reviewed. Every INV maps to at least
  one VT-*/TF-*/MD-*/MC-* test:
  - INV-1 ↔ VT-1, VT-2
  - INV-2 ↔ TF-3 (corrupt file recovery), VT-1 (preexisting
    trust honored after restart-equivalent reload)
  - INV-3 ↔ TF-1
  - INV-4 ↔ design constraint; engine test reads bytes once
    via QFile::readAll
  - INV-5 ↔ VT-2 (falls back without failure)
  - INV-6 ↔ VT-6
  - INV-7 ↔ VT-3 (SHA pin matches), VT-4 (SHA pin re-prompts on
    edit), VT-5 (no-pin trusts forever)
  - INV-8 ↔ design constraint; MD-1 verifies the modal fires
  - INV-9 ↔ VT-7 (session cache)
  - INV-10 ↔ design; back-compat shim documented in § 4.5
- ✅ Cross-doc: composes with ANTS-1372 (caller-cwd gate). The
  two protect different planes — ANTS-1372 ensures the writer
  is the right project, ANTS-1337 ensures the project's
  bespoke config is trusted. Both are necessary in the hostile-
  clone scenario (caller cwd matches the hostile project, but
  the project's content shouldn't auto-run).
- ✅ RAM budget § 9 still accurate — < 16 KB persistent for 100
  trust entries; sub-ms verify-time cost; modal blocks only the
  rc handler thread.

Status: **CLEAN.** Ready for Loop 2 implementation pass.

**Loop 2 — review-contract, 2026-09-26 (armed by ANTS-5411, 3d7f617f:
INV-2 and § 9 now say the client re-reads a changed trust file):**

- Two `review-lane` lanes, every question in each, over the scrubbed
  copy and a packet of verifytrust.{h,cpp}, verifyengine.cpp,
  remotecontrol_review.cpp, verifytrustmodal.h, secureio.h and the
  verify_trust_gate test contract. Both lanes disclosed arriving with
  the git snapshot naming 3d7f617f's subject; neither read § 11.
- `Loop 2 — Q1 5 · Q2 3 · Q3 0 · Q4 0` (verified 8 / fixed 0 /
  dismissed 0). Seven of eight found by both lanes independently.
- **None landed on the armed change.** Lane B checked the new INV-2
  and § 9 text against `reloadIfChanged` / `stampOf` and found it
  matches. Share of findings inside the armed span: 0 / 8.
- All eight are pre-existing drift against shipped code, so they exit
  at the blast-radius rule, filed rather than looped:
  ANTS-5465 (seven: § 4.5 signature and shim vs INV-10, § 4.1
  `trusted_repos` schema, § 4.2 DenyOnce vs INV-5/INV-9, INV-9's cache
  key, INV-1 vs ANTS_VERIFY_TRUST_AUTOTRUST, VT-6 vs the test contract,
  § 4.6 pseudo-code error checks) and ANTS-5464 (surfaced to the user:
  since ANTS-4932, `verify_changes` is served by ants-mcpd with a
  prompt-less client, so § 4.3 / § 8's prompt is unreachable from a
  Claude session; a design choice).
- Packet defect, the orchestrator's: it gave the subject's length as
  the original's 531 lines, not the scrubbed copy's 456. Line numbers
  up to § 11 match, which both lanes checked.

Status: gated change CLEAN; tail filed as ANTS-5464 and ANTS-5465.
