# ANTS-1355 token_usage v2 — review record

The loop log for [`docs/specs/ANTS-1355-token-usage-v2-wrap-overhead.md`](../specs/ANTS-1355-token-usage-v2-wrap-overhead.md).

## 10. Cold-eyes review log

- **Loop 0** (initial draft, 2026-05-15): drafted against
  ANTS-1284's spec template + verified anchors:
  - `claudeintegration.cpp:2700–2705` (wrap/no-wrap branch)
  - `claudeintegration.cpp:2710–2713` (v1 `recordCall` site)
  - `claudeintegration.cpp:2763–2795` (`wrapMcpData` body)
  - `tokenusageengine.{h,cpp}` (v1 engine surface)
  - `remotecontrol.cpp:3157–3199` (`cmdTokenUsage` builder)
  - ANTS-1360's `mcpTraceTimer` declared at line 2642–2643.

- **Loop 1** (2026-05-15, verify pass): re-verified every cited
  line number against current source; counted escape-substitution
  growth precisely; recounted registered tool providers.
  **0 CRITICAL + 1 HIGH + 0 MEDIUM + 1 LOW** finding — both
  addressed:
  - HIGH-1 (INV-13 tool-count drift): spec claimed 27 registered
    tools; `grep -c registerToolProvider src/mainwindow.cpp`
    returned **32** (the count has grown since ANTS-1284's spec).
    With the +1 inline `get_session_info`, current total is 33.
    INV-13 RAM math corrected: 33 × 128 B = ~4.2 KiB.
  - LOW-1 (INV-3 escape count): wrap-escape substitution claimed
    `</ants_mcp_data>` (16 chars) → `<ants_mcp_data_escaped/>`
    "26 chars". Actual target is 24 chars. Replaced with the
    correct count + added the tool-name HTML-entity encoding to
    the non-shrinking proof.

- **Loop 2** (2026-05-15, verify pass): **CLEAN.** Re-read every
  cited line in `claudeintegration.cpp` (2310, 2642, 2643,
  2700–2705, 2713, 2718, 2721, 2730–2732, 2763–2795) — all match.
  Re-counted registerToolProvider hits (32). Re-walked INV-3's
  non-shrinking proof against `wrapMcpData` body. Re-read § 2.5
  diff against `claudeintegration.cpp:2700–2719` — the v2
  `recordCall` insertion site is unchanged. No new findings.
  Sign-off ready.

## Cold-eyes loop log

- **impl (2026-11-15)** — no reviewer was dispatched; implementation fold-back. ANTS-6378: ANTS-4457's open-tag scrub shortens a body, so INV-3's non-shrinking proof no longer held and wrap_bytes went negative. INV-3 now counts the frame alone; § 2.2's table row and § 2.5's sketch follow it.
