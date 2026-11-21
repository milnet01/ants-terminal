# ANTS-1727 — review-dialog foundation: `LlmClient`, `LlmDispatcher`, `ReviewDialogBase`, shared dispatch-brief composer — review loop log

Record for [`../specs/ANTS-1727-review-dialog-foundation-llmclient.md`](../specs/ANTS-1727-review-dialog-foundation-llmclient.md) § Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-05-21, independent cold reviewer + author self-review).**
  No blockers. Folded: (a) `m_currentReply` ref corrected
  `aidialog.cpp:69` → `aidialog.h:69`; (b) "widget-free" reframed as
  discipline + new INV-16 source-grep guard (`ants_core_lib` links
  Qt6::Widgets, so it is not a lib boundary); (c) `coldeyesengine.h:61`
  re-cited as the 12-spec-lane cap with 17 stated as the derived total;
  (d) ctor arg-order aligned to AuditDialog (`cwd, parent, config`).
  Self-review (pre-reviewer) had already fixed the fold-in double-
  allocation hazard (subclass owns `performFoldIn`; base provides
  helpers) and added `dispatchOne` for the synthesis follow-up.
  Clean pass.
- **Sign-off (2026-05-21, user).** Specs approved. One direction folded:
  add `BriefDispatch::inlineRelevantSections` (INV-17) — a section-aware
  slicer — so cold-eyes large cross-reference docs are sliced to relevant
  sections rather than head-truncated (user: "a smarter slice is
  required"). Status → accepted.
- **Implementation (2026-05-21).** Shipped under TDD; all 1347 tests green.
  Deviations from the as-written spec, each user-or-test-driven:
  - **INV-11 / G-15 relocation** (user-approved): a clean fence-kernel
    extraction empties the literals G-15 grepped inside
    `assembleBriefForDispatch`, so G-15 was relocated to grep
    `briefdispatch.cpp::fenceBody` + assert the call. Behaviour unchanged.
  - **`fenceBody` label param**: added a defaulted `label="file"` 3rd arg
    so the indie-review standards-doc loop keeps its `=== standard: …`
    header — the refactor is byte-identical to the prior inline form.
  - **AiDialog scrub kept in the dialog** (INV-1): see § 2.5 — moving scrub
    into `LlmClient` would break `ai_context_redaction`'s unchanged-pass
    requirement; only the network moved. `scrubSecrets=false` avoids a
    double scrub.
  - **Test-seam statics**: `LlmClient::buildRequestBody` + `accumulateCapped`
    + `kMaxBytes` added so INV-3/INV-4 verify scrub + cap without a network.
  - **Bundle placement**: `LlmClient`/`LlmDispatcher` are `Q_OBJECT`s;
    referencing them pulls `ants_core_lib`'s aggregated moc, which
    co-references `RemoteControl`/`VtStream` → unlinked GUI libs in a
    Core-only bundle. So `llm_client`/`llm_dispatcher` live in `test_claude`
    and `review_dialog_base` in `test_dialogs` (full lib set);
    `brief_dispatch_fence` (moc-free) + `config_ai_review_concurrency` stay
    in `test_core`.
  - **dtor-virtual fix**: `~ReviewDialogBase` disconnects the dispatcher
    before `cancelAll()` — otherwise a `cancelAll`-emitted `allFinished`
    routes to the pure-virtual `onAllReportsCollected` after the derived
    dtor ran ("pure virtual method called"). Caught by `review_dialog_base`.
