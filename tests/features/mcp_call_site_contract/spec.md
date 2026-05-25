# Feature spec: every `registerToolProvider` call declares its CallerCwdContract at the call site (ANTS-1419)

ANTS-1404 (Phase 3a) added a per-tool `CallerCwdContract`
classification table at `ClaudeIntegration::callerCwdContractFor`.
The table is the source of truth the dispatcher consults to refuse
`caller_cwd_required` for tools that mutate per-project state.

The original `registerToolProvider(name, handler)` signature said
nothing about the security contract — a contributor adding a new
tool registration was free to omit the classification step in
`callerCwdContractFor` and ANTS-1417's coverage test caught the gap
only at test-run time, not at compile time. ANTS-1419 closes that
gap by hoisting the contract into the registration signature:

```cpp
m_claudeIntegration->registerToolProvider(
    "tool_name",
    ClaudeIntegration::CallerCwdContract::Required,
    [this](const QJsonObject &args) -> QString { ... });
```

The dispatcher consults the contract stored on the registered
entry (single source of truth at the call site). The static table
is preserved as a back-compat accessor for tests and for tools
dispatched inline (`get_session_info`, `tool_info`), with a
runtime drift assertion in `registerToolProvider` that compares
the passed value against the table.

## Invariants

- **INV-1 / registerToolProvider takes a contract.** The header
  declares `void registerToolProvider(const QString &name,
  CallerCwdContract contract, ToolHandler handler);`. Both
  `CallerCwdContract` and `ToolHandler` parameter names must be
  present, in that order.
- **INV-2 / every call site passes a contract.** Every
  `m_claudeIntegration->registerToolProvider("<name>", ...)`
  invocation in `src/mainwindow.cpp` MUST include
  `ClaudeIntegration::CallerCwdContract::<Required|Optional|
  TabSpecific|ProcessGlobal>` as the second positional argument,
  before the handler lambda.
- **INV-3 / drift assertion present.** The body of
  `ClaudeIntegration::registerToolProvider` MUST call
  `callerCwdContractFor(name)` and fail loudly when the passed
  contract disagrees — the static table remains as a back-compat
  consumer (tests + inline tools) and silent drift between the
  two sources of truth would re-introduce the very class of bug
  this refactor closes.
- **INV-4 / value type carries the contract.** `m_toolProviders`
  value type is `RegisteredTool` (or equivalent struct/pair)
  bundling `handler` and `contract`, so the dispatcher can read
  the call-site declaration in one map lookup.
- **INV-5 / dispatcher prefers the stored contract.** The
  `tools/call` dispatch site in `claudeintegration.cpp` consults
  `m_toolProviders[toolName].contract` before falling back to the
  static `callerCwdContractFor` table — registered tools use the
  call-site value; inline tools (`get_session_info`, `tool_info`)
  use the table.
- **INV-6 / drift refused in every build (ANTS-1834).** The drift
  branch in `registerToolProvider` must contain a compiled `return;`
  after the `Q_ASSERT_X`, so a Release build (where `Q_ASSERT_X`
  compiles out under `NDEBUG`) refuses the registration instead of
  falling through and registering with a possibly-wrong contract. A
  `Required` tool silently registered as `Optional` would bypass the
  `caller_cwd_required` refusal at dispatch — exactly the class of
  hole this gate exists to close. Debug builds additionally abort via
  `Q_ASSERT_X`.

## Test scope

Source-scrape against `src/claudeintegration.h`,
`src/claudeintegration.cpp`, and `src/mainwindow.cpp`. No GUI /
RemoteControl / dispatcher instantiation needed.

Failure mode the test catches: a future contributor adds a new
tool registration in the old `(name, handler)` shape, or passes
a contract that disagrees with `callerCwdContractFor`. The
test fails fast before the build ships.

## Out of scope

- Removing the static `callerCwdContractFor` table. Phase 2
  cleanup; tests outside `mcp_call_site_contract` still consume
  the static API.
- Enforcing TabSpecific contracts at dispatch time. ANTS-1415
  tracks that work.
