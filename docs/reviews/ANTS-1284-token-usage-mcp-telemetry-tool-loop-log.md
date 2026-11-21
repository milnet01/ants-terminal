# ANTS-1284 — `token_usage` MCP telemetry tool — review loop log

Record for [`../specs/ANTS-1284-token-usage-mcp-telemetry-tool.md`](../specs/ANTS-1284-token-usage-mcp-telemetry-tool.md) § 11. Cold-eyes review log.

## Cold-eyes loop log

- **Loop 0** (initial draft, 2026-05-14): drafted by author
  against the ANTS-1289 + ANTS-1290 spec templates +
  `claudeintegration.cpp:1187, 2005–2027` (initialize and
  dispatch handlers) + the existing `m_toolProviders` registry
  shape at `claudeintegration.h:185, 308`.

- **Loop 1** (2026-05-14, agent-driven verify pass): completed.
  Verified against `wc -c ROADMAP.md` (593 632 B),
  `wc -c verification-before-completion/SKILL.md` (4 201 B),
  `wc -c writing-plans/SKILL.md` (6 100 B), grep for anchor
  lines in `claudeintegration.cpp` (`// ANTS-1289` at 1873,
  `// ANTS-1290` at 1914, `result["tools"] = tools;` at 1990 —
  insertion line for ANTS-1284 is the gap between 1988 and
  1990), exact sed-read of dispatch site lines 1985–2030
  (`bool toolHandled = false;` at line 2005;
  closing brace of else-if branch at line 2027; `if
  (toolHandled)` at line 2029), exact sed-read of initialize
  handler at line 1187 (`if (method == "initialize")` opens at
  1187, closes at 1196), bundle-name confirmation (`test_audit`
  + `test_claude` are real CMake targets at
  `CMakeLists.txt:782, 876`), and re-read of both prior MCP
  test files for delimiter strategy.
  **2 CRITICAL + 2 HIGH + 1 MEDIUM + 2 LOW** findings — all
  addressed in this revision:
  - CRIT-1: `roadmap_query` baseline was 504 000 (back-of-envelope
    from `~123 K tokens × 4`); actual file is 593 632 B. § 2.4
    rewritten with the wc-measured anchor; round to 594 000 for
    headroom. § 5 INV-4 + INV-5 tests updated to use the new
    constant.
  - CRIT-2: spec claimed the verify_changes test "was tightened
    to the generic-anchor pattern in the ANTS-1290 commit." The
    test actually hardcodes `// ANTS-1290` (see
    `test_mcp_verify_changes_tool.cpp:79`). Rewrote § 5 with the
    correct structural reason for stability under insertion
    (both prior tests stable for different reasons).
  - HIGH-1: INV-2 specified `QString::size()` for `bytes_out`
    (UTF-16 code-unit count), which contradicts the `bytes_out`
    field name on multi-byte responses. Switched to
    `.toUtf8().size()` (UTF-8 wire bytes) — ~1–5 µs alloc cost,
    documented in § 1 cost table.
  - HIGH-2: Tool count off-by-one in INV-8 (claimed "26 + future
    headroom"; actual after this work is 27 — 25 outward
    delegates verified at `mainwindow.cpp:3671–3869` +
    `get_session_info` inline + `token_usage` itself).
  - MED-1: QString hash overhead in INV-8 was 64 B/entry; Qt6's
    `QString` is ~48 B (24 B inline header + ~24 B heap for a
    12-char tool name in UTF-16) + 24 B `ToolCounter` + ~24 B
    `QHash` slot overhead = ~96 B. Worst case adjusted to
    ~6 KiB (still well under any practical concern).
  - LOW-1: § 5 test 4's synthetic over-baseline call used
    600 000 B response; spec baseline is now 594 000 → bumped
    test to 700 000 to keep the floor assertion meaningful.
  - LOW-2: minor wording polish ("subtotal per MCP call" caption
    updated from `< 1 µs` to `< 6 µs` to reflect the `.toUtf8()`
    alloc).
  Sign-off ready after these edits; loop 2 will re-verify.

- **Loop 2** (2026-05-14, agent-driven verify pass): **CLEAN
  after one HIGH followup.** Re-verified loop-1 anchors against
  current source: `grep -c registerToolProvider src/mainwindow.cpp`
  returned 24 (loop 1 claimed 25 — off-by-one), so INV-8 was
  corrected from "27 total after this work" to **26** (24
  outward delegates + 1 inline `get_session_info` + 1
  `token_usage` after this work). Re-read
  `test_mcp_verify_changes_tool.cpp:71–85` to confirm the
  named-successor delimiter shape (hardcoded `// ANTS-1290` with
  fallback to `result["tools"] = tools;`) — matches the loop-1
  rewrite of § 5. Re-read `claudeintegration.cpp:2027–2040` to
  confirm `if (toolHandled)` lives at line 2029 (matches the
  refactor's structural assumption). Re-confirmed `wc -c` on
  ROADMAP.md (593 632 B) and both skill markdowns (4 201 B,
  6 100 B). No further changes to baselines, schema, or test
  plan. **1 HIGH followup + 0 MEDIUM + 1 LOW** finding — both
  handled:
  - HIGH-followup: tool count off-by-one (loop-1 fix said 27,
    actual 26). INV-8 corrected.
  - LOW: cross-checked all line-number citations in the spec
    against `grep -n` and `sed -n` results — all current.
  Sign-off ready.
