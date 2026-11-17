# ANTS-2022 — apply_edits: review loop log

## Cold-eyes loop log

- **Loop 1 (2026-06-05) — `/cold-eyes`, fresh cold reviewer.** 0 CRITICAL.
  Verified + fixed: (HIGH) the spec claimed field-for-field "flip_batch
  parity" but `flip_batch` emits `flipped`/`flipped_count` with
  `skipped[]` of `{locator_index, code, error}` (`remotecontrol.cpp`
  ~4616; `roadmap_log_flip_batch` test confirms `flipped_count`) → softened
  every "parity" to the soft-skip **posture** and stated the file-edit
  field names (`applied`/`files_written`/`{index,path,reason}`) are
  deliberately distinct. (HIGH) `"write_failed"` was used as a per-edit
  skip reason, but `write_failed` is an existing **top-level** refusal code
  in `mcp-error-codes.md` (used by `build_status`/`spec_log`/`test_results`)
  → renamed the skip reason to `"commit_failed"` and reconciled § 2.3 /
  § 2.4 / § 7. (MEDIUM) the `applyRepair` ambiguity guard
  (`auditautofix.cpp:126`) is `removeLine`-gated whole-**line** uniqueness,
  not substring → reframed it as an analogous-but-narrower precedent, with
  `apply_edits` doing substring uniqueness. (MEDIUM) pinned the file read,
  the 4 MiB `too_large` gate, and trailing-newline preservation to the
  `cmdApplyEdits` wrapper (the pure helper sees only in-memory `contents`).
  (MEDIUM) added the closed `skipped[].reason` enumeration in one place.
  No top-level code minted (re-used `bad_path`/`bad_args`/`caller_cwd_required`).

- **Loop 2 (2026-06-05) — `/cold-eyes`, fresh cold reviewer.** 0 CRITICAL
  / 0 HIGH; loop-1 fixes all held (the flip_batch field-name divergence,
  the `commit_failed`-not-`write_failed` rename, and the helper/wrapper
  layer split all read clean on the cold pass). Verified + fixed: (MEDIUM)
  refusal-code emission sites were implicit → stated `caller_cwd_required`
  is dispatcher-emitted while `bad_args`/`bad_path` are handler-emitted.
  (MEDIUM) `applied[]` per-file vs `skipped[]` per-edit asymmetry could
  mislead → stated `applied[]` is file-scoped, carries no input `index`,
  and per-edit applied-index recovery is out of scope. (open-q → INV-11)
  added the `edits_total == edits_applied + edits_skipped` balance
  invariant and pinned commit-failure accounting (a failed commit moves
  that file's edits to `skipped[]`/`commit_failed`, decrementing
  `edits_applied`). (open-q) pinned `replace_all:true` + 0 occurrences →
  `not_found` (never a silent no-op).

- **Loop 3 (2026-06-05) — `/cold-eyes`, fresh cold reviewer.** 0 CRITICAL
  / 0 HIGH; all loop-1/2 fixes held on the cold read (the `commit_failed`
  rename, layer split, count-balance INV-11, and closed reason-set all read
  clean). Verified + fixed: (MEDIUM) § 4 / INV-9 framed the 4 MiB cap as
  "the existing gate" but `kReadToolMaxBytesCeiling` has no write-side
  consumer yet → reworded to "reuse the read-tool constant; `cmdApplyEdits`
  is its first write-side consumer". (LOW) unified the `applyRepair` EOL
  citation on `auditautofix.cpp:110-134` (was `:112-134` in INV-8). (LOW)
  softened INV-11's commit-failure test note so the read-only-dir fixture
  covers both `QSaveFile` open and commit failure paths.

- **Loop 4 (2026-06-05) — `/cold-eyes`, fresh cold reviewer.** 0 CRITICAL
  / 0 HIGH / 0 MEDIUM — reviewer called the spec "implementation-ready".
  Folded the residual LOW/INFO: (LOW) the § 2.5 worked example straddled
  the edit-count vs substitution-count axes (`replacements:3` with
  `edits_applied:1`) → reworked to a coherent 3-edit example (1 applied with
  `replacements:1`, 2 skipped). (INFO) cited the short-write guard line
  (`auditautofix.cpp:140-141`) at § 2.4. (wording) reworded INV-11's
  commit-failure clause from "move from `applied[]` to `skipped[]`" to
  "reported in `skipped[]`, never in `applied[]`", matching the file-scoped
  `applied[]` shape.

- **Loop 5 (2026-06-05) — `/cold-eyes`, fresh cold reviewer (confirming
  pass).** 0 CRITICAL / 0 HIGH / 0 MEDIUM — "accurate against current
  source on every load-bearing citation and internally coherent". Folded
  the LOW/INFO residue: (LOW) the §2 dispatch snippet's
  `CallerCwdContract::Required` → qualified `ClaudeIntegration::` to match
  the real call site; (INFO) annotated the §2.5 worked example so the
  0-based index 0 (the applied edit) is explicit. Open question (doc-sync,
  not a defect): the ROADMAP lane tags for ANTS-2022 are `mcp, workspace`
  as §7 claims — confirmed against the bullet. Spec converged → **accepted**.

- **impl-6467 (2026-11-15) — implementation fold-back; no reviewer
  dispatched.** ANTS-6467: a file changed between apply_edits' read and
  its write is skipped with the new reason `concurrent_edit`, not
  overwritten (INV-17, § 2.3, § 2.4). Loop rows moved out of the spec
  into this file. Test seen red first
  (`McpApplyEdits.Ants6467ConcurrentEditIsNotOverwritten`).
- **impl-6534 (2026-11-17)** — implementation fold-back; no reviewer dispatched. ANTS-6534: a file that is not valid UTF-8 is skipped with the new reason `not_utf8`, never rewritten; a leading BOM is kept (§ 2.3 skip list and its closed reason set). Test seen red first (`McpApplyEdits.Ants6534NonUtf8FileIsSkippedAndLeftIntact`); the BOM test was killed by a mutant dropping the BOM flag.
