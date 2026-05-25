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
- **INV-8 (ANTS-1853) / `caller_cwd_required` distinguishes an empty
  arguments object from a single missing field.** When the dispatcher's
  Required gate fires AND the whole `arguments` object is empty (the
  call's parameters were dropped in transit — an intermittent tools/call
  serialisation drop), the envelope MUST set `arguments_empty:true` and
  the error text MUST steer the caller to resend the entire call, not
  just re-add `caller_cwd`. The dispatcher MUST also emit a
  `DebugLog::Claude` diagnostic recording `arguments_empty` + the keys
  that WERE present, so a recurrence is root-causable (empty → dropped
  upstream; non-empty-without-caller_cwd → genuine caller error). The
  `code` stays `caller_cwd_required` (taxonomy unchanged). Anchor:
  `ANTS-1853` in `src/claudeintegration.cpp` tools/call dispatch.
- **INV-9 (ANTS-1857) / the dropped-payload steer is size-aware.**
  Since the drop correlates with payload size (large structured
  appends drop; small calls don't), the empty-arguments steer MUST go
  beyond "resend": it names the size root cause ("too large") and the
  two caller-side mitigations — shrink the call, or write the content
  with the Edit tool. The `roadmap_log` tool descriptor MUST also carry
  a proactive `SIZE NOTE (ANTS-1853)` so a caller keeps appends small
  BEFORE the drop happens. Anchors: `ANTS-1857` + `Edit tool` in the
  dispatch steer, `SIZE NOTE (ANTS-1853)` in the roadmap_log
  descriptor.

## Test scope

Source-scrape against `src/claudeintegration.cpp` and
`src/remotecontrolgate.cpp` for the `hint` field emission and its
content. Functional check on `RcGate::gateErrorEnvelope` that the
hint appears only on `cwd_missing`. Mirrors the ANTS-1404
caller_cwd_contracts test pattern.
