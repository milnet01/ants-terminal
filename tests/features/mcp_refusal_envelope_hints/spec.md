# Feature spec: MCP refusal envelopes name caller_cwd_info as diagnostic path (ANTS-1418)

ANTS-1400 shipped `caller_cwd_info` — a diagnostic verb that
surfaces the focused-tab + caller-cwd resolution decision.
ANTS-1404's `caller_cwd_required` refusal envelope tells the caller
"pass your $PWD as caller_cwd" but doesn't mention the diagnostic
verb that lets them *confirm* their `caller_cwd` would resolve
correctly — exactly the case the verb was built to address
(symlinked project roots, worktree checkouts, container bind-mounts
that cause `caller_cwd` to pass but route to the wrong project).

Same for the ANTS-1336 / ANTS-1372 RcGate `cwd_missing` refusal.

## Invariants

- **INV-1 / `caller_cwd_required` envelope carries a `hint` field
  that names `caller_cwd_info`.** When `callerCwdContractFor(tool) ==
  Required` and the caller passes empty `caller_cwd`, the dispatcher
  returns `{ok:false, code:"caller_cwd_required", error:"…",
  hint:"…caller_cwd_info…"}` — the hint MUST contain the literal
  tool name `caller_cwd_info` so a caller scanning the envelope
  finds the diagnostic path. Anchor: `ANTS-1418` in
  `src/claudeintegration.cpp::processTools`.
- **INV-2 / `cwd_missing` (RcGate) envelope carries the same hint.**
  When `RcGate::checkCallerCwd` returns with `errorCode ==
  "cwd_missing"` and `gateErrorEnvelope` builds the JSON, the
  resulting envelope MUST include the same `hint` field referencing
  `caller_cwd_info`. Anchor: `ANTS-1418` in
  `src/remotecontrolgate.cpp::gateErrorEnvelope`.
- **INV-3 / non-cwd_missing RcGate errors don't carry the hint.**
  `cwd_bad`, `no_project`, `cwd_mismatch` — those refuse for reasons
  the diagnostic verb wouldn't directly help with (the caller has
  a `caller_cwd`, it just doesn't resolve / no project / mismatch).
  Test asserts the hint is absent on those branches so future drift
  doesn't bolt it onto every gate error indiscriminately.

## Test scope

Source-scrape against `src/claudeintegration.cpp` and
`src/remotecontrolgate.cpp` for the `hint` field emission and its
content. Functional check on `RcGate::gateErrorEnvelope` that the
hint appears only on `cwd_missing`. Mirrors the ANTS-1404
caller_cwd_contracts test pattern.
