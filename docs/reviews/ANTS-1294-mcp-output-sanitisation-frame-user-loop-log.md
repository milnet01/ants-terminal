# ANTS-1294 — MCP output sanitisation (frame user-supplied text as data) — review loop log

Record for [`../specs/ANTS-1294-mcp-output-sanitisation-frame-user.md`](../specs/ANTS-1294-mcp-output-sanitisation-frame-user.md) § 11. Cold-eyes review log.

## Cold-eyes loop log

- **Loop 0** (initial draft, 2026-05-14): drafted by author
  against the ANTS-1284 spec template +
  `src/claudeintegration.cpp:2253–2306` (dispatch handler) +
  the existing `m_toolProviders` registry shape at
  `src/claudeintegration.h:187–326` + the 30 provider
  registrations at `src/mainwindow.cpp:3671–3917` + the
  source-grep test patterns at
  `tests/features/mcp_extra_tools/test_mcp_extra_tools.cpp`.

- **Loop 1** (2026-05-14, agent-driven verify pass): **CLEAN
  after three count/line-number fixes.** Re-verified:
  - `grep -cE '^\s*m_claudeIntegration->registerToolProvider\(' src/mainwindow.cpp`
    = 30 (loop 0 said "24 separate provider lambdas" in § 2.2 —
    corrected to 30; § 2.2 tally now reads "29 of the 31
    dispatched — 30 registered providers + 1 inline
    `get_session_info`, minus 2 control-plane exemptions").
  - `sed -n 2290,2305p src/claudeintegration.cpp` confirmed
    `if (toolHandled) {` at **line 2294**, `haveResult = true;`
    at line 2301. Loop 0 wrote "2278–2295" (the wider if/else-if
    span) — § 2.4 narrowed to "2294–2301" matching the actual
    `if (toolHandled)` block being modified.
  - `grep -n ToolHandler src/claudeintegration.h` confirmed the
    `using ToolHandler` typedef at **line 187** (not 185 as the
    loop 0 log said).
  - `grep -nE 'registerToolProvider\("[^a-z_"]' src/mainwindow.cpp`
    returned **zero matches** — § 2.1 claim that tool names match
    `^[a-z][a-z0-9_]+$` holds.
  - `grep -n "wrapMcpData\|ants_mcp_data" src/*.h src/*.cpp`
    returned **zero matches** — no collision with existing
    identifiers / string literals.
  - `grep -n "^### Conventions" CLAUDE.md` matched at line 198 —
    § 2.5's "Conventions section" anchor exists.
  - `grep -n "static QString" src/claudeintegration.h` confirmed
    the existing pattern (`sessionPathForCwd`, `decodeProjectPath`,
    `encodeProjectPath` at lines 126, 210, 211) — adding
    `wrapMcpData` as another `static QString` is consistent.
  No further changes to invariants, surface, or test plan.
  Sign-off ready after this revision; loop 2 will re-verify.

- **Loop 2** (2026-05-14, agent-driven independent verify pass):
  **CLEAN.** Re-pinned every cited line number against current
  source:
  - `claudeintegration.cpp:2253` is the `else if (method ==
    "tools/call")` opening (matches § 11 Loop-0 anchor).
  - `claudeintegration.cpp:2257–2264` is the `makeTextContent`
    lambda body (matches § 4's "verified by reading" citation).
  - `claudeintegration.cpp:2294` is `if (toolHandled) {` (matches
    § 2.4 + § 8 + REG-3 anchors).
  - `claudeintegration.cpp:2301` is `haveResult = true;` (last
    statement inside the block — matches the "2294–2301" span).
  - `claudeintegration.h:187` is `using ToolHandler` (matches
    Loop-1's correction from 185).
  - `claudeintegration.h:326` is `std::map<QString, ToolHandler>
    m_toolProviders;` (matches Loop-0's lower bound of the cited
    range).
  - `mainwindow.cpp:3671` is the first `registerToolProvider`
    call (`get_scrollback`); `mainwindow.cpp:3917` is the last
    (`session_memory`) — registry span confirmed.
  - The 30 tool names enumerated by
    `grep -oE '"[^"]+"' on registerToolProvider lines` all match
    `^[a-z][a-z0-9_]+$`. No hyphens, no leading digits, no
    upper-case. § 2.1 quote-escaping argument holds.
  - JSON-RPC wire-format sanity: the wrap places literal `<`/`>`
    and an embedded `\"` inside the `text` field of a JSON
    object. `QJsonDocument::toJson(Compact)` escapes the embedded
    `"` to `\"` automatically (Qt6 documented behaviour). `<` /
    `>` are valid JSON string content per RFC 8259 § 7. No
    additional escape needed in the helper.
  Sign-off achieved.
