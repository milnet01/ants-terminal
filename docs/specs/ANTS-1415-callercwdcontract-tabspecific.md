# ANTS-1415 — `CallerCwdContract::TabSpecific` enforcement (Phase 3b)

**Status:** spec draft (2026-05-20).
**Kind:** security.
**Source:** in-session-2026-05-16 (deferred from ANTS-1404 § Out of
scope); roadmap bundle plan pull 40.

Phase 3a (ANTS-1404, shipped 2026-05-16) built the per-tool
`CallerCwdContract` framework and enforced the `Required` category at
the MCP dispatch site. It classified five tools `TabSpecific` but left
them unenforced, deferring the refusal rule to a spec pass because the
routing-vs-anchoring semantics overlap with ANTS-1392. Phase 3b is that
pass: enforce the `TabSpecific` category so a per-tab read tool no
longer silently falls back to the focused tab when the caller supplies
no routing key.

## 1. Problem

The `TabSpecific` tools read **per-tab** terminal state. Each resolves
its target tab through `MainWindow::terminalForCaller(callerCwd)` (or,
for two of them, an explicit `tab` index first). When the caller passes
**no routing key**, `terminalForCaller("")` hits the
`ResolvedRoot::Source::EmptyFallback` case and returns
`focusedTerminal()` (`mainwindow.cpp:2776–2780`). So a Claude session in
project B that calls `get_scrollback` without `caller_cwd` gets back
whatever project A's tab Ants happens to have focused — the exact
cross-tenant leak shape ANTS-1404 closed for `Required` tools, still
open for these six.

Six tools are classified `TabSpecific` today (five at ANTS-1404
spec-time plus `recent_errors`, added by ANTS-1301):

| Tool | Routing keys it actually consumes |
|------|-----------------------------------|
| `get_text` | explicit `tab` index **OR** `caller_cwd` (cmdGetText, ANTS-1392) |
| `recent_errors` | explicit `tab` index **OR** `caller_cwd` (cmdRecentErrors, `remotecontrol.cpp:1087–1093`) |
| `get_scrollback` | `caller_cwd` only (`terminalForCaller`) |
| `get_last_command` | `caller_cwd` only |
| `get_environment` | `caller_cwd` only |
| `get_cwd` | `caller_cwd` only (echo/canonicalise; else focused-tab cwd) |

The split matters: the four cwd-only tools **ignore** a `tab` arg, so a
stray `tab:N` must not count as a routing key for them (it would let a
caller bypass the gate and still get the focused-tab fallback).

## 2. Goal

Enforce `TabSpecific` at the dispatch site: refuse a call that carries
**no routing key the tool can use**, with a distinct refusal code, BEFORE
the rate-limit check and the idempotent-read cache — mirroring the
Phase 3a `Required` branch. "No usable routing key" means:

- `caller_cwd` is empty, **AND**
- the tool is not a tab-routing tool, **OR** the tool is a tab-routing
  tool but no integer `tab` arg was supplied.

Equivalently: accept iff `caller_cwd` is non-empty, **or** the tool is
`get_text`/`recent_errors` and `tab` is an integer.

This preserves every legitimate use (ANTS-1392 caller_cwd routing,
explicit-`tab` reads for the two tab-routing tools) and closes only the
no-routing-key focused-fallback path.

## 3. Design

### 3.1 Refusal code — `tab_or_cwd_required`

A new code in `docs/standards/mcp-error-codes.md` § 3 (Caller-cwd
contract). `caller_cwd_required` does not fit: for `get_text` /
`recent_errors`, `caller_cwd` is *not* strictly required (an explicit
`tab` also routes), so a code that says "caller_cwd required" would
misdirect those callers. `tab_or_cwd_required` (19 chars, ≤ 24 per the
standard's naming rule; matches the `cwd_*` family) names the actual
condition: provide a tab index or a caller_cwd.

### 3.2 Dispatch-site enforcement

In the `tools/call` branch of `processTools`
(`claudeintegration.cpp`), immediately **after** the Phase 3a
`Required` refusal block (~line 5884) and **before** the rate-limit
check (~line 5893):

```cpp
// ANTS-1415 — Phase 3b: TabSpecific contract enforcement. The per-tab
// read tools route on caller_cwd (ANTS-1392) and, for two of them, an
// explicit `tab` index. With NO usable routing key they fall back to
// the focused tab — the cross-tenant leak ANTS-1404 closed for
// Required tools, deferred from Phase 3a. Refuse before the rate-limit
// + cache, mirroring the Required branch. See
// docs/standards/mcp-error-codes.md § 3.
if (!toolHandled && contract == CallerCwdContract::TabSpecific) {
    const bool tabIsRoutingKey = tabSpecificAcceptsTabIndex(toolName);
    const bool hasTab =
        tabIsRoutingKey && argsObj.value(QStringLiteral("tab")).isDouble();
    if (callerCwd.isEmpty() && !hasTab) {
        QJsonObject env;
        env["ok"]    = false;
        env["code"]  = QStringLiteral("tab_or_cwd_required");
        env["error"] = (tabIsRoutingKey
            ? QString("%1: pass a `tab` index or `caller_cwd` (your "
                      "$PWD) to identify the target tab (ANTS-1415). "
                      "Without either, this per-tab tool falls back to "
                      "whichever tab Ants has focused — which may belong "
                      "to a different project.")
            : QString("%1: pass `caller_cwd` (your $PWD) to identify the "
                      "target tab (ANTS-1415). Without it, this per-tab "
                      "tool falls back to whichever tab Ants has focused "
                      "— which may belong to a different project."))
            .arg(toolName);
        env["hint"] = QStringLiteral(
            "call mcp__ants__caller_cwd_info with your $PWD to confirm "
            "which tab Ants would route this call to; tab_list gives "
            "tab indices");
        QJsonObject ex;
        ex[QStringLiteral("caller_cwd")] = QStringLiteral("<your $PWD>");
        env[QStringLiteral("example")] = ex;
        responseText = QString::fromUtf8(
            QJsonDocument(env).toJson(QJsonDocument::Compact));
        toolHandled    = true;
        dispatchResult = QStringLiteral("tab_or_cwd_required");
    }
}
```

`contract` and `callerCwd` are already in scope from the Phase 3a
block. Setting `toolHandled = true` makes the existing
`!toolHandled`-guarded provider-dispatch + cache paths short-circuit
(ANTS-1404 INV-7/DISP-4 already widened those guards to `!toolHandled`).
`dispatchResult` routes the refusal into `recordDispatch`'s failed-call
accumulator (ANTS-1454), so `token_usage` and `mcp_trace` count it.

### 3.3 Helper — `tabSpecificAcceptsTabIndex`

A small file-local helper (anonymous namespace in
`claudeintegration.cpp`, beside the dispatch site):

```cpp
// ANTS-1415 — the two TabSpecific tools whose handler honours an
// explicit `tab` index as an alternate routing key (cmdGetText,
// cmdRecentErrors). The other four (get_scrollback, get_last_command,
// get_environment, get_cwd) ignore `tab`, so it must NOT count as a
// routing key for them — else a stray tab:N would bypass the Phase 3b
// gate and still fall back to the focused tab.
bool tabSpecificAcceptsTabIndex(const QString &toolName) {
    return toolName == QLatin1String("get_text") ||
           toolName == QLatin1String("recent_errors");
}
```

Named (not inlined) so the routing-key knowledge is greppable and the
regression test can assert its membership.

### 3.4 What stays unchanged

- **Classifications.** All six tools stay `CallerCwdContract::TabSpecific`
  in `callerCwdContractFor` and at their registration call sites. Phase
  3b adds enforcement, not reclassification — the ANTS-1419 call-site
  drift check and the Phase 3a classification tests keep passing.
- **Schemas.** The tools/list schemas are left as-is. `caller_cwd` is
  NOT added to any `required[]` array, because for `get_text` /
  `recent_errors` an explicit `tab` is a valid alternative — a schema
  `required:["caller_cwd"]` would wrongly reject a legitimate
  `tab`-only call. Enforcement is at the dispatch site, which knows the
  tab-or-cwd disjunction; the schema can't express it cleanly.
- **C++ call paths.** The status-bar widgets and other in-process
  callers reach these tools through `terminalForCaller` / the `cmd*`
  handlers directly, NOT through the MCP `tools/call` dispatch. The
  Phase 3b gate is at the MCP dispatch only, so internal callers are
  unaffected.

## 4. Invariants

| # | Statement |
|---|-----------|
| 1 | A new refusal code `tab_or_cwd_required` is added to `docs/standards/mcp-error-codes.md` § 3 with a 1-line meaning + example. |
| 2 | The `tools/call` dispatch refuses a `TabSpecific` tool with `{ok:false, code:"tab_or_cwd_required", error, hint, example}` when `caller_cwd` is empty AND (`!tabSpecificAcceptsTabIndex(tool)` OR `tab` is not an integer). |
| 3 | The refusal runs AFTER the Phase 3a `Required` branch and BEFORE the rate-limit check and idempotent-read cache lookup. |
| 4 | `tabSpecificAcceptsTabIndex` returns true for exactly `get_text` and `recent_errors`, false for everything else. |
| 5 | A `TabSpecific` call WITH a non-empty `caller_cwd` is never refused by this branch (regardless of `tab`). |
| 6 | A `get_text` / `recent_errors` call WITH an integer `tab` and empty `caller_cwd` is NOT refused (explicit-tab routing preserved). |
| 7 | A `get_scrollback` / `get_last_command` / `get_environment` / `get_cwd` call with a stray integer `tab` but empty `caller_cwd` IS refused (a `tab` they ignore is not a routing key). |
| 8 | The refusal sets `toolHandled = true` and `dispatchResult = "tab_or_cwd_required"`, so the provider lambda and cache are bypassed and `recordDispatch` counts the refusal as failed (mirrors ANTS-1454). |
| 9 | All six tools remain classified `CallerCwdContract::TabSpecific` in `callerCwdContractFor` and at their registration call sites (no reclassification). |
| 10 | The error message names both the tool and the missing routing key(s), and is tab-aware (mentions `tab` only for the two tab-routing tools). |

## 5. Test

`tests/features/mcp_tabspecific_contract/` (source-scrape, label
`features;fast` — mirrors the Phase 3a contract test; runtime MCP
socket dispatch needs the full event loop and isn't warranted):

1. **Refusal block present (INV-2).** The `tools/call` region contains
   `code` / `"tab_or_cwd_required"` and refuses on
   `contract == CallerCwdContract::TabSpecific` with the
   `callerCwd.isEmpty() && !hasTab` condition.
2. **Ordering (INV-3).** The `tab_or_cwd_required` literal appears
   AFTER the `caller_cwd_required` literal and BEFORE the
   `isIdempotentReadTool` cache-gate literal.
3. **Helper membership (INV-4).** `tabSpecificAcceptsTabIndex` is
   defined and its body contains `get_text` and `recent_errors` and not
   the four cwd-only tool names.
4. **dispatchResult routed (INV-8).** The refusal branch sets
   `dispatchResult = QStringLiteral("tab_or_cwd_required")`.
5. **No reclassification (INV-9).** `callerCwdContractFor` still
   classifies all six as `TabSpecific`.
6. **Error-code doc (INV-1).** `docs/standards/mcp-error-codes.md`
   contains the `tab_or_cwd_required` row.

## 6. Migration

Contract break for callers of the six tools that omit every routing
key. Same posture as Phase 3a: refuse immediately (the leak is
cross-tenant data exposure; warn-then-allow ships it another release).
The refusal is structurally distinct (`code:"tab_or_cwd_required"`) and
the fix is mechanical — add `caller_cwd` (works for all six) or a `tab`
index (for `get_text`/`recent_errors`). Local skills don't invoke these
six tools with neither key (they are MCP reads issued by the Claude
session, which knows its own `$PWD`). CHANGELOG `Security` entry names
the six tools and the new code.

## 7. Out of scope

- **`ProcessGlobal` / `Optional` enforcement.** Unchanged — those
  categories intentionally accept-and-ignore / accept-and-fallback.
- **Schema `required` marking.** Left as-is (§ 3.4); the tab-or-cwd
  disjunction isn't cleanly expressible as JSON-schema `required`.
- **Adding a `tab` index to the four cwd-only tools.** They route on
  `caller_cwd` by design; giving them an explicit `tab` is a separate
  feature, not a security fix.
- **ANTS-1420 (drop the deprecated `cwd` schema field).** Bundled with
  pull 40 in the roadmap plan, but its own text gates removal to 0.7.93
  ("drops in 0.7.93", "0.7.93 release checklist"). The `cwd` deprecation
  first *ships* in 0.7.92 (the next release); removing it now would land
  in 0.7.92 with zero migration window. Deferred to the 0.7.93
  checklist — see ROADMAP ANTS-1420.

## 8. RAM & build cost

Zero new state. One helper + one dispatch-site `if` block (~30 lines)
in `claudeintegration.cpp`. No new TU. Per-call cost: one map lookup
(already done for Phase 3a) + one string compare in the helper.

## 9. Doc-drift fold-in (same commit)

- Flip `ROADMAP.md` ANTS-1415 to ✅.
- Add the `tab_or_cwd_required` row to `docs/standards/mcp-error-codes.md` § 3.
- CHANGELOG `Security` entry naming the six tools + the new code.
- ANTS-1420 stays 📋, annotated as deferred to 0.7.93.
