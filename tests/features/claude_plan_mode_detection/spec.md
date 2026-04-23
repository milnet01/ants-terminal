# Feature: Claude Code plan-mode detection

## Contract

When Claude Code is running in **plan mode** (the user toggled it via
Shift+Tab), the status bar MUST reflect this — the `planModeChanged(true)`
signal is wired to a UI indicator that tells the user "this turn is
advisory, no files will be modified." If the signal never fires, the
user sees regular-mode UI while Claude refuses every edit tool call —
extremely confusing.

## External anchor

This spec is anchored to the **real** Claude Code JSONL schema, observed
on disk in `~/.claude/projects/<slug>/<session>.jsonl` as of Claude Code
v2.1.87 (verified 2026-04-23):

```
{"type":"permission-mode","permissionMode":"default","sessionId":"…"}
{"type":"permission-mode","permissionMode":"plan","sessionId":"…"}
{"type":"permission-mode","permissionMode":"acceptEdits","sessionId":"…"}
```

The field name on these standalone toggle events is `permissionMode`, not
`mode`. The valid values observed in the wild are `default`,
`acceptEdits`, `plan`, and `bypassPermissions`.

## Invariants

1. **Most-recent-toggle wins.** Given a JSONL transcript containing
   multiple `{"type":"permission-mode", …}` events, the LAST one in the
   file determines current plan-mode state.

2. **`permissionMode == "plan"` → `planModeChanged(true)`.** When the
   last toggle event has `permissionMode` equal to `"plan"` (case-
   sensitive, exact match), `ClaudeIntegration::planModeChanged(true)`
   MUST be emitted.

3. **Any other value → `planModeChanged(false)`.** `"default"`,
   `"acceptEdits"`, `"bypassPermissions"`, missing field, empty string —
   all evaluate to NOT-plan-mode.

4. **No toggle events → plan mode stays whatever it was.** If the
   transcript contains no `{"type":"permission-mode"}` events at all,
   the signal must NOT fire on re-parse (no spurious toggles).

5. **Field name is `permissionMode`, not `mode`.** The parser must read
   the correct field per the external schema above. This is the
   specific regression this test guards: up to and including 0.7.11 the
   code read `value("mode")`, which never matched the real schema, so
   plan-mode detection never fired in production.

## How this test anchors to reality

The JSONL fixtures in `test_claude_plan_mode.cpp` use the exact shape
observed in live `~/.claude/projects/*.jsonl` files. If Anthropic
changes the schema in a future Claude Code release, this test will
fail — which is the correct signal (the parser needs updating) rather
than silent drift.

## Regression history

- **Introduced:** at some point before 0.7.0 when the plan-mode
  feature was first wired. No working implementation ever shipped —
  the signal was dead code.
- **Discovered:** 2026-04-23, via the multi-agent independent review
  sweep (`/indie-review`). The Claude Code subsystem reviewer checked
  field names against live JSONL and flagged the mismatch.
- **Fixed:** 0.7.12 (read `permissionMode`, exact-match on `"plan"`).
