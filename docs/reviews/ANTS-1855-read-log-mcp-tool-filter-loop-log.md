# ANTS-1855 — `read_log` MCP tool (filter a log file, return only matching lines) — review loop log

Record for [`../specs/ANTS-1855-read-log-mcp-tool-filter.md`](../specs/ANTS-1855-read-log-mcp-tool-filter.md) § Cold-eyes loop log.

## Cold-eyes loop log

- **doc-fix (2026-10-19) — ANTS-5770, not a review loop: no reviewer
  was dispatched.** Brings the spec in line with shipped code: § 2.3
  adds `lines_oversize_skipped` and `time_budget_exhausted`;
  `lines_dropped` is present iff above zero; source citations name
  current files, without line numbers; § 6 lists the covered ids.
  Records what was built, so no new gate.
- **8-fix (2026-10-19) — not a review loop: no reviewer was dispatched.**
  Loop 7's surfaced Q1 (multi-line private keys pass a per-line scrub)
  resolved by the user 2026-10-19: blank key blocks. § 2.6 step 6 tracks
  BEGIN/END markers across scanned lines, INV-14's test gains a block
  fixture, § 5 updated. The gate's own fix pass, so no new gate.
- **Loop 8 (2026-10-19) — `review-contract --genre spec`, 2 cold lanes,
  each holding every question. Capped: loop 2 of this run, the spec
  cap.** Q1 0 · Q2 0 · Q3 0 · Q4 1; verified 1, fixed 1, dismissed 2.
  Q4 (one lane): nothing tested that `cmdReadLog` passes the redaction
  key, so a forgotten wire-up would pass every test → the `Options`
  field defaults to on, and INV-14 adds a source-scrape (§ 6 follows).
  Dismissed: re-capping after scrubbing can briefly exceed INV-12's
  heap bound (scrub-at-emission is a recorded ruling and the build is
  the same either way); the test contract lacks INV-14 (INV-14 names
  its test). Own-fix share 1/1, an unpropagated consequence of loop 7's
  key decision rather than a repair of a repair: a calm cap. Armed-span
  share 4/5 across both loops. This loop's fix is read by no lane.
- **Loop 7 (2026-10-19) — `review-contract --genre spec`, 2 cold lanes,
  each holding every question; gating the ANTS-5707 amendment.**
  Q1 1 · Q2 1 · Q3 1 · Q4 0, plus 1 mechanical; verified 4, fixed 3,
  surfaced 1, dismissed 0. Both lanes found all three. Mechanical
  (`spec_lint` `missing_section`): no § Reload, which
  `docs/standards/specs.md` requires on amendment → added. Q2: step 5
  fixed `returned` before step 6's re-cap → `returned` and
  `lines_dropped` now count after step 6. Q3: whether
  `claude.mcp_redact_secrets` governs redaction, and which layer
  scrubs, was unstated → the key governs it, `cmdReadLog` reads it per
  call, `ReadLog::filter` scrubs; INV-14 and § 7 follow. Q1, surfaced
  for a decision: a per-line scrub never matches a multi-line PEM
  private-key block, so § 5's "matches `SecretRedact`'s patterns" is
  false for that shape. Out of radius, not fixed: § 2.3's envelope
  omits `lines_oversize_skipped` and `time_budget_exhausted`.
- **amend (2026-10-19) — ANTS-5707, not a review loop: no reviewer was
  dispatched.** Adds § 2.6 step 6 (redact), the `redacted` envelope
  field, INV-14 and an out-of-scope bullet on shape-only redaction.
  `review-contract` gates it next.
- **Loop 6 (2026-05-25) — `/cold-eyes`, fresh cold reviewer (confirming
  pass).** **0 CRITICAL / 0 HIGH.** Loop-1..5 fixes all held. Only
  residue: a cite-style nit the reviewer itself called defensible
  (`debuglog.cpp:162-163` — body + loop-log already agree; dismissed),
  an accurate `pathvalidation.h:20-21` cite (info-only; dismissed), and
  one clarity fold-in (Q1): clarified `max_bytes` bounds the **serialised**
  `lines[]` contribution per line (quotes/comma/escapes), not just raw
  text, consistent with `capJsonArrayToBytes`. Spec converged → marked
  **accepted**.
- **Loop 5 (2026-05-25) — `/cold-eyes`, fresh cold reviewer.** Loop-1..4
  fixes all held; no CRITICAL. Fixed: (HIGH) `path` echo form was
  undefined and the example used a tilde form while `logFilePath()`
  returns an absolute path → stated the echoed `path` is absolute and
  fixed the example. (MEDIUM) `tail_selected` looked like an emitted
  field → marked intermediate. (LOW) `since` debuglog cite →
  `:162-163` (bracket prefix at :162); added a DST mis-order caveat;
  trimmed a duplicated example value. Not changed: `Kind: feature` is
  valid (live `roadmap_log`/`validKinds` enum + the ANTS-1855 bullet use
  it) — roadmap-format.md's table is the stale side, filed as ANTS-1866.
- **Loop 4 (2026-05-25) — `/cold-eyes`, fresh cold reviewer.** Loop-1/2/3
  fixes all held; no CRITICAL. Fixed: (HIGH) `bytes_cap_clamped` was
  promised by §2.3 prose + INV-8 but missing from the envelope JSON
  example → added. (MEDIUM) `cursor` is a byte offset while the cited
  `get_scrollback` cursor is a monotonic push counter → clarified read_log
  shares only the protocol shape, not the encoding; also pinned the
  literal `"malformed_cursor"` reason. (MEDIUM) `pathvalidation.h:21`
  resolved-empty cite → `:20-21`. (LOW) added a worked `since` example.
  Findings shrinking each loop (CRITICAL 2→0→0→0).
- **Loop 3 (2026-05-25) — `/cold-eyes`, fresh cold reviewer.** Loop-1/2
  fixes all held. New findings verified + fixed: (HIGH) an existing
  array-capper `capJsonArrayToBytes` (`remotecontrol.h:246`) keeps the
  front / drops the newest + runs post-hoc → named it and its wrong-end
  behaviour so an implementer neither reinvents nor misuses it.
  (MEDIUM/RAM) INV-12 / § 4 heap bound was unproven when `tail` is
  omitted (all survivors accumulate before the cap) → redesigned the cap
  as a **streaming drop-oldest deque** bounded by `max_bytes`, so peak
  heap ≤ cap regardless of `tail` or file size; INV-8/INV-12/§4 + § 2.6
  step 4 updated. (MEDIUM) `tail:N` pinned per-call (not cumulative
  across `since_cursor` polls). (LOW) INV-1 test grounded on
  `XDG_DATA_HOME` temp dir (no code injection seam); INV-6/INV-7 wording
  aligned to §2.2 (local-time prefix, clamp-not-refuse); § 7 CLAUDE.md
  note clarified (prose sentence, not a bullet). No CRITICAL.
- **Loop 2 (2026-05-25) — `/cold-eyes`, fresh cold reviewer.** Loop-1
  fixes all held (none recurred). New, deeper findings verified + fixed:
  (HIGH) `"rotated"` `stale_reason` was claimed to mirror get_scrollback
  but that tool emits `malformed_cursor`/`counter_regressed`/`ring_wrapped`
  → reworded so only the soft-fallback *posture* mirrors; `"rotated"` is
  read_log's own file-offset reason. (HIGH) `makeFieldsProp` is a
  block-local lambda (`claudeintegration.cpp:1578`), not a grep-able
  function → noted the schema add must live in that builder scope. (HIGH)
  `tail` said `bad_args` on out-of-range but the cited `lines` exemplar
  *clamps* → changed to clamp, not refuse. (MEDIUM) in-root-but-absent
  path → `not_found` via empty `check.resolved` (not `bad_path`/`check.bad`);
  `since` debuglog cite :162→:163; "16 MiB cap"→"ceiling" (+ note
  `kGetTextDefaultBytesCap`=1 MiB); `lines_dropped` formula via
  `tail_selected`; `bad_cwd`/non-resolving caller_cwd handled upstream;
  edge cases (empty file, `since`+`since_cursor`, partial-line counting)
  added to § 2.6.
- **Loop 1 (2026-05-25) — `/cold-eyes`, 2 cold reviewers.** Verified +
  fixed: (CRITICAL) `registerBuiltinTools` does not exist → repointed to
  the `tools/list` builder block; (CRITICAL) the `mainwindow.cpp:3851`
  dispatch exemplar is `recent_errors`/**TabSpecific**, wrong for a
  Required tool → repointed to `:3989` (`get_git_status`). (HIGH) filter
  + `tail` + `max_bytes` + `since_cursor` ordering was underspecified →
  added § 2.6 pipeline + count semantics. (HIGH) INV-8 collided with the
  string-based `trimScrollbackForGetText` → stated read_log does its own
  line-array accounting on the **read-tool** cap family
  (`kReadToolDefaultBytesCap` 512 KiB / `kReadToolMaxBytesCeiling` 4 MiB,
  not get_text's 16 MiB). (HIGH) INV-10 said a bad cursor → `bad_args`
  but the cited `get_scrollback` soft-falls-back → changed to
  `cursor_stale` + full re-read. (MEDIUM) `caller_cwd_required`
  :6410→:6411; hardcoded `/home/ants/...` path → `~/...`; `since`
  local-time/no-offset caveat added; ROADMAP lanes-drift note added.
  (LOW) `tail` out-of-range → `bad_args`; envelope optional keys shown.
  Dismissed (unverified): Kind `feature` "invalid" — it is in the live
  `roadmap_log` enum and the ANTS-1855 bullet uses it; roadmap-format.md
  §3.5.3's table is the stale side. Provenance ANTS-1293-vs-1348 — the
  spec cites no cap ID, so no error.
- **Loop 0 (2026-05-25) — grounding + self-review.** Verified every
  cited symbol against live source: `DebugLog::logFilePath()`
  (debuglog.cpp:135), `PathValidation::validatePath` + `struct Check`
  (pathvalidation.h:46/24 — corrected from a wrong :33 in the first
  draft), `callerCwdContractFor` / `caller_cwd_required` /
  `CallerCwdContract::Required`, `isFieldProjectionTool`
  (mcpprojection.cpp:11), `kGetTextDefaultBytesCap` (= 1 MiB,
  remotecontrol.h:162), `registerToolProvider(... Required)`
  (mainwindow.cpp:3989). Internal-consistency check: filter order in
  § 2.2 matches INV-4..7; cap behaviour in § 2.3 matches INV-8; counts
  in § 2.3 match INV-9. No duplication with existing tools
  (recent_errors = scrollback, get_scrollback = grid; neither reads a
  file). RAM caps named (§ 4). Three design forks parked in § 8 for
  sign-off rather than decided unilaterally. Open for a reviewer pass.
