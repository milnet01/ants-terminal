# ANTS-1901 — Master Ants-MCP on/off toggle with graceful degrade — review loop log

Record for [`../specs/ANTS-1901-master-ants-mcp-off-toggle.md`](../specs/ANTS-1901-master-ants-mcp-off-toggle.md) § 9. Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-06-25)** — 2 reviewers (accuracy-vs-code; consistency +
  cross-doc). Verified + folded: 1 CRITICAL (Settings tab was named "AI
  Assistant"; the integration toggles live on the **General** tab —
  matches code + ANTS-1897's own correction), 2 HIGH (user-facing
  `mcp_disabled` error string carried the wrong tab; INV-2 claim
  exceeded its grep test surface), 3 MEDIUM (`cachedConfig()`/mtime
  mechanism mis-described; missing degrade-matrix table; CLAUDE.md
  block-name drift), 3 LOW (Pairs-with imprecision, `settingsChanged`
  phrasing, error-code category unnamed). Dismissed/unverified: 0
  (the no-`setMcpEnabled`-collision and line-citation open questions
  were confirmed, not refuted).
- **Loop 2 (2026-06-25)** — same 2 reviewers, cold. Verified + folded:
  1 CRITICAL (INV-6 still named "AI Assistant" — loop 1 fixed §2.5 + the
  error string but missed the invariant; now "General"), 2 HIGH (INV-5
  did not test guard *placement* before the caller_cwd gate; menubar
  deferral lacked traceability), 4 MEDIUM/LOW (the §2.2 snippet elided
  but didn't note the preserved INV-14 comment block; INV-2 still
  falsifiable-gap on a stray export; awkward anchor parenthetical;
  `mcp_disabled` naming-rule conformance unstated; Pairs-with/ANTS-1900
  phrasing; `shellState` 1494→1493). Verified-but-out-of-scope: the
  `tool_not_found` (code) vs `unknown_tool` (`mcp-error-codes.md` §5)
  drift — a pre-existing code/doc mismatch, roadmapped separately, not
  folded here. `cachedConfig()` mtime-reload confirmed at
  `claudestatuswidgets.cpp:75-79` (immediate-degrade claim holds).
- **Loop 3 (2026-06-25)** — same 2 reviewers, cold. Verified + folded:
  1 CRITICAL (the **ROADMAP** ANTS-1901 body + Layman still said "AI
  Assistant" — the spec was right but the planning doc it elaborates was
  unreconciled; corrected the ROADMAP and added a § 7 cross-doc line),
  2 HIGH (INV-8 read as a runtime invariant when it's cross-doc
  completeness — reframed; INV-5's byte-offset placement test was
  brittle against multiple `caller_cwd_required` strings — now names the
  specific emit site at `:8739`), 4 MEDIUM/LOW (§ 2.4 cited a comment as
  "cache lookup"; orientation-load `1027`→`1027-1028`; added the
  matrix-row→INV mapping note; hoisted the anchor caveat to a doc-wide
  convention, folding the ANTS-1897 stale-line cross-note; added the
  INV-2-vs-ANTS-1897 test-ownership note). Dismissed: 1 (a "mid-sentence
  break artifact" that is just markdown soft-wrap — renders as one
  sentence).
- **Loop 4 (2026-06-25)** — same 2 reviewers, cold. 0 CRITICAL. Verified
  + folded: 2 HIGH (the ROADMAP still promised "(Settings + menubar)"
  while § 6 defers the menubar — split the menubar to its own follow-up
  **ANTS-2172** and reconciled the ROADMAP headline/part-(c)/Layman so
  scope matches; INV-8's grep tested bare presence, not placement under
  category 5 — now placement-checked), 3 MEDIUM/LOW (§ 2.4 cited the
  `caller_cwd_required` envelope at `:8738`, aligned to INV-5's `:8739`
  emit site; glossed `model_switch_ledger` with the real classes
  `ModelSwitchLedger`/`ModelNearMissLedger`; category-5 anchor `:113` →
  the heading string). Dismissed: 2 (a `mirrorMaster` "starts at 314"
  claim — it is at 315, verified; a matrix-vs-INV-5 anchor "inconsistency"
  that is two valid anchors for the same guard). Out-of-scope, routed to
  follow-ups: ANTS-1897's own internal General-vs-AI-Assistant string
  drift, and the `tool_not_found` (code) vs `unknown_tool` (taxonomy)
  mismatch.
- **Loop 5 (2026-06-25, `--max-loops` cap)** — same 2 reviewers, cold.
  0 CRITICAL. Verified + folded: 3 HIGH (INV-4's test grepped a
  `sendToPty("/model ` token that does not exist — the call wraps
  `QStringLiteral("/model ")` across lines; retargeted. INV-8 housed a
  doc-sync check in the runtime-INV namespace — moved to § 7 as a
  release-coupling requirement, INV set is now INV-1…INV-7. The ROADMAP
  body still cited the pre-drift `startMcpServer` ctor line `:3867` —
  corrected to `:4046-4048`). 3 MEDIUM/LOW (§ 2.4 comment cite
  `:8685`→`:8686`; INV-2 reworded to a count==1 assertion, delegating
  the else-binds-nothing half to INV-3; §2.5 hard-line softening left to
  the doc-wide anchor convention). Both reviewers confirmed the § 9 log
  matches current text — no prior fix regressed.
