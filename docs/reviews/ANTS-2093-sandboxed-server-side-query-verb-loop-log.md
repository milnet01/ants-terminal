# ANTS-2093 — Sandboxed server-side query verb (`project_query`) — review loop log

Record for [`../specs/ANTS-2093-sandboxed-server-side-query-verb.md`](../specs/ANTS-2093-sandboxed-server-side-query-verb.md) § Cold-eyes loop log.

## Cold-eyes loop log

Two independent cold reviewers per loop (no author intent), each briefed on the
spec + cited code (`luaengine.{cpp,h}`, `pluginmanager.cpp`, `mcpprojection.cpp`,
`remotecontrol.cpp`, `claudeintegration.{cpp,h}`) + cross-ref docs. Every
finding verified against source before fixing; loops 2+ briefed cold.

**Loop 1 — 1C / 2H / 5M / 3L verified, all fixed.**
- C (threading freeze) — a single uninterruptible C call (`string.gsub`) on the
  GUI-thread dispatcher (`claudeintegration.h:613–614`, "single-threaded …
  Qt main thread") would freeze the UI; the hook can't preempt pure-C
  (`luaengine.cpp:134–136`). Fixed: §2.4 pins the query VM to a **worker
  thread** (ANTS-1750 `moveToThread` + detach, `pluginmanager.cpp:481–499`) with
  a bounded join; INV-4 + §4 restated to "GUI never blocked beyond
  `timeout_ms` + grace".
- H1 (threading contradiction) — "synchronously on the dispatch path" clashed
  with the worker-thread engine design; resolved by the same §2.4 contract.
- H2 (build flag) — spec said `-DANTS_LUA=OFF`; the real flag is
  `ANTS_LUA_PLUGINS` (`CMakeLists.txt:378/499/1676`). Fixed §2.5 + added the
  catalogue-absence test (§6).
- M1 failure-envelope shape unstated → added the `{ok:false,error,code}` note
  (§2.1). M2 recursion depth unnumbered → pinned **32** (§2.4/INV-6). M3 line
  citations off (`:139`→`:141`, `:181`→`:180`, `:177`→`:175`,
  `:135–137`→`:134–136`) → corrected. M4 gate precedence unstated →
  master-before-feature rule added (§2.5/INV-8). M5 threat-model re-derived
  ADR-0004 → §1.1 tightened to the Lua-specific risk.
- L1 `base`→`base (the _G table)`; L2 PLUGINS.md target section named;
  L3 invalid-UTF-8 refusal test added (§6). INFO (isOffloadEligible set grows
  9→10) noted, no action.

**Loop 2 — 1H / 4M / 6L verified & fixed; 2 dismissed.**
- H (threading honesty + RAM accuracy) — both reviewers flagged the worker-thread
  model CRITICAL. On verification the *design* is sound (worker + bounded join +
  detach is the right bounded-vs-unbounded trade), but the *prose* overclaimed:
  "GUI never blocked beyond timeout+grace" reads as "not blocked" (the join runs
  on the GUI thread and **does** stall it, bounded), and §4 wrongly said a
  detached worker "frees on the 10 MiB cap" — it is held as a zombie until
  process exit (`pluginmanager.cpp:138–150`, `:492`, `:125`). Restated INV-4 +
  §2.4 + §4 honestly (bounded GUI stall = `timeout_ms` + 250 ms grace; zombie
  leak ≤ 10 MiB + thread/pathological-query, bounded not unbounded).
- M1 `runQuery`/`QueryResult`/`registerQueryApi(root)` were named but not
  signed → added a `cpp` surface block (§2.4). M2 gate precedence was
  unfalsifiable → cited the master-gate site `claudeintegration.cpp:8688`
  (§2.5/INV-8). M3 §2.3 offered `validatePath` "or" the inline pattern as
  "equivalent" → mandated `validatePath` (inline shown only as its internal
  logic). M4 `list` sort order unspecified → pinned byte/codepoint,
  locale-independent (§2.2/INV-7; guards the ANTS-2120 collation hazard).
- L: depth-32 surfaced in the marshalling table + circular-table note;
  failure-envelope codes marked "5 new"; refusals (`ok:false`) never offloaded;
  config clamp/cap rationale; `grace` pinned at 250 ms; `base`→`luaopen_base`
  registered as `_G`; PLUGINS.md § Sandbox Boundaries named.
- **Dismissed (verified not-a-defect):** (a) "INV-9 offload-scrape test fails
  before the verb is integrated" — that *is* the intended fail-first TDD
  discipline (§6 states every new assertion must fail pre-implementation). (b)
  "`registerQueryApi()` isn't written yet" — the nature of a pre-implementation
  spec; the actionable part (its signature) was the M1 fix.

**Loop 3 — 1H / 3M / 2L verified & fixed; several dismissed.**
- H/M (validatePath pinning) — §2.3 still offered a "wrapper if the signature
  can't take a root" hedge. Verified `PathValidation::validatePath(rawPath,
  rootCanonical, toolName, paramName, allowOutsideRoot)` (`pathvalidation.h:46–50`)
  **already** takes an arbitrary root → pinned the direct call, dropped the
  wrapper/deferred-choice hedge.
- M (error-code category) — §7 said "add a category" without naming it; both
  reviewers flagged the gap. Named **"6 — Query execution"** (new, after § 5)
  with the rationale for not folding into 1–5.
- M (zombie accounting) — §4 "bounded, not unbounded" could mislead; clarified
  the leak is bounded *per query* but **additive across a session** (dispatch
  serialises execution, not teardown).
- M (marshaller contract) — `runQuery` now states it wires the per-call budget
  (`setPcallBudgetMs`+`startPcallBudget`), that a snippet *raise* → `query_error`
  (never `null`; `null` is clean no-return only), and that the cap is the
  **UTF-8 byte count**.
- L: `QueryResult` code/error "set iff !ok" invariant comment; §6 gained the
  depth-bound boundary cases (32 ok / 33 → `query_error`) and a note that
  master-gate precedence is a dispatch-level test, not a `cmdProjectQuery` unit.
- **Dismissed:** (a) recurring "codes not in taxonomy / verb not registered /
  not in `isOffloadEligible` yet → CRITICAL" — the nature of a
  pre-implementation spec; those are exactly what the implementing commit adds
  under the §6 fail-first discipline. (b) `loadScript` cited at `:381` — verified
  **:379** is correct (`bool LuaEngine::loadScript(...)`; :381 is the
  `if (!m_state)` line). (c) `mcp-behavioural-notes.md` "may not exist" — it
  exists (referenced in CLAUDE.md, ANTS-2088). (d) `list` "byte ⇒ already
  locale-independent" redundancy — emphasis kept deliberately given the
  ANTS-2120 collation hazard (reviewer concurred "not wrong").

**Loop 4 — 0C / 0 factual-H / 2 implementability + 4L verified & fixed; rest dismissed.**
Both reviewers returned **0 CRITICAL**; the two "HIGH"s were framing/placement
clarifications, not factual defects.
- (framing) §2.4 said "reusing the ANTS-1750 pattern" — clarified it is a **new,
  ephemeral per-call worker** *modelled on* that pattern, not the plugin
  manager's long-lived threads or a shared pool (an implementer might otherwise
  reuse the wrong machinery).
- (placement) §2.5 now pins the feature gate as the **first statement inside
  `cmdProjectQuery`**, before `caller_cwd`/arg validation, so feature-off →
  `query_disabled` regardless of arg state (testable at the unit level).
- L: §6 adds an explicit **circular-table** case (`t.self=t` → `query_error` via
  the depth bound); §1.1 marks a same-UID **local attacker** explicitly
  out-of-scope; `.git/` skip rationale widened (metadata/perf); base lib reworded
  (`luaopen_base`, exposed as `_G`); INV-9 scrape reworded to a literal
  `"project_query"` grep.
- **Dismissed:** timeout-ceiling rationale "missing" (already added in loop 2,
  §2.5); `loadScript` "should be :381" (verified **:379** correct); category-6
  append "may break tooling" (mcp-error-codes.md is convention-enforced, a plain
  append); `mcp-behavioural-notes.md` structure (adequate for a doc-impact list).

**Loop 5 — clean pass (0C / 0H / 0M; 2 citation-precision nits folded).**
Reviewer B returned **zero** findings ("implementable as-is, production-ready");
reviewer A returned 0C/0H and two citation-precision nits, both with "factual
accuracy 100%": `isOffloadEligible` cited at the signature line `:45` → corrected
to the verb-name body `:46–54`; the master-gate cite `:8688` (a comment) →
widened to the block `:8688–8706`. No substantive defect surfaced. **Converged.**
