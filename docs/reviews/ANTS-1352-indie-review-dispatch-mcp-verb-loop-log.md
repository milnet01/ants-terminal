# ANTS-1352 — `indie_review_dispatch` MCP verb: server-side reviewer orchestrator — review loop log

Record for [`../specs/ANTS-1352-indie-review-dispatch-mcp-verb.md`](../specs/ANTS-1352-indie-review-dispatch-mcp-verb.md) § 9. Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 0 (draft, 2026-05-17).** First draft.
- **Loop 1 (2026-05-17).** Cold-eyes subagent surfaced 3 CRITICAL,
  4 HIGH, 5 MEDIUM, 4 LOW. All folded:
  - **C-1** (Config::aiModel default = "llama3"): § 2.1 + § 3.3 now document the footgun and echo `resolvedModel` in envelope.
  - **C-2** (API key in response body): INV-21 added; § 3.4 redaction step pinned.
  - **C-3** (prompt-injection via source bodies): INV-22 added; 4-backtick fence treatment in `assembleBriefForDispatch` (§ 3.2).
  - **H-1** (`ai_not_configured` is also new): § 2.5 calls out both new codes.
  - **H-2** (INV-15 mis-categorisation): non-http scheme now → `bad_args`.
  - **H-3** (assembleBrief inlines "fetches if needed"): new engine fn `assembleBriefForDispatch` introduced; § 3.2 specifies it.
  - **H-4** (INV-9 contradiction + untestable): rewritten; probe accessor introduced; P-1 test added.
  - **M-1** (RAM block streaming claim): § 5 rewritten honestly.
  - **M-2** (no idempotent retry): § 8 future-work bullet added.
  - **M-3** (INV-11 usage shape ambiguity): precedence rule pinned (OpenAI > Anthropic > zero).
  - **M-4** (system prompt not pinned): inlined verbatim in § 3.1.
  - **M-5** (no cache INV): INV-20 added.
  - **L-1** (auditrunner line ref): citation softened.
  - **L-2** ("log as v2"): rewritten as "file as 💭 ANTS-1352-followup".
  - **L-3** (§ 1 self-ref typo): rewritten.
  - **L-4** (grep stability): INV-1/INV-2 carry `// ANTS-1352` comment anchors.
  - **Suggestions:** threading note added to § 2.3 (MCP socket thread; UI main loop unaffected); INV-23 added (no absolute paths in prompt — `caller_cwd` doesn't leak).
- **Loop 2 (2026-05-17).** Cold-eyes subagent verified loop 1 fixes and surfaced 1 HIGH, 3 MEDIUM, 3 LOW:
  - **H-new-1** (orphan duplicate § 3.1/§ 3.2 block from the fold-in): the original pre-revision text remained after § 3.5. Deleted lines 354-375; now only the revised § 3.4/§ 3.5 carry the api-key handling + atomic-write rules.
  - **M-new-1** (truncate-vs-redact order): § 3.5 now pins **redact-then-truncate** with a one-sentence rationale.
  - **M-new-2** (`assembleBriefForDispatch` invocation layer): § 3.2 now names the MCP handler (`cmdIndieReviewDispatch` in `remotecontrol.cpp`) as the caller; dispatcher receives pre-assembled `LaneRequest.brief` strings only.
  - **M-new-3** (OpenAI-wins vs Anthropic accuracy): accepted as documented. Real-world proxies emitting both shapes are rare; if a user reports drift, file as 💭 ANTS-1352-followup.
  - **L-new-1** (24 KiB standards docs ×14 lanes redundancy): noted; § 5 budget already absorbs it. v2 could hoist to system message — log as future-work if a sweep ever exceeds memory budget in practice.
  - **L-new-2** (fence-replacement notation): cleanly parses, no spec change needed.
  - **L-new-3** (cross-project concurrency of `inFlightCountForTest`): acceptable as documented; the probe accessor returns process-global count, which is the desired behaviour under INV-4's per-(verb, cwd) gate.
  - Sign-off: **CLEAN** after H-new-1 fix + M-new-1/M-new-2 pins.
