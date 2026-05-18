# ANTS-1579 — `verify_changes` `timeout_sec` end-to-end plumb verification

**Status:** implementing
**Source:** cross-session-report-2026-05-17 (Vestige /test-audit Issue #1)
**Pull:** 12 (MCP timeout-ergonomics fold-in, 0.7.92)

## 1. Problem

Vestige 2026-05-17 called
`verify_changes({gates:["build"], timeout_sec:900})` and saw
`MCP error -32000: Ants MCP transport: timed out` — even though the
build itself finished in well under a minute via `Bash → cmake --build`.
The symptom was the client-side transport timeout (Claude Code's
~60 s request cap), not the tool-side gate budget. **ANTS-1525**
already named that distinction in the description; **ANTS-1492**
already fixed the per-gate division bug.

The remaining gaps:

1. **No regression test pins the positive plumb path.** Existing
   `verify_changes_engine` tests assert the timeout-kill case (Inv2:
   1 s budget kills a `sleep 30` gate) but no test asserts the
   complement — that a caller-supplied `timeout_sec` actually leaves a
   gate enough headroom to *complete*. ANTS-1492 fixed the perGateSec
   division silently; a future regression that re-introduces the bug
   wouldn't be caught.
2. **The description's asymmetry note is buried.** Today the
   transport-vs-tool distinction lives in one of the schema's property
   descriptions. Vestige read the full tool description and still
   filed a confused bug — moving the asymmetry to the top-level
   description (next to the ANTS-1525 plumb note) makes it the first
   thing a caller sees when picking the tool.

## 2. Fix

Two changes, both small:

- **New live feature test** under
  `tests/features/mcp_verify_changes_timeout_headroom/`. Writes a
  synthetic `.ants/verify.json` with `build: sleep 1`. Drives
  `runVerify` with `timeout_sec=3` (and the minimum-floor overrides
  so the 10 s floor doesn't dominate). Asserts: gate ran, gate
  passed, `exitCode == 0`, `durationSec < 3.0`. This proves
  end-to-end that `opts.timeoutSec=3` → `perGateSec ≥ 3` →
  `runOneGate(..., perGateSec)` leaves the 1 s gate enough headroom.
  Pairs with the existing Inv2 negative test (kill-on-expiry) for
  bidirectional plumb coverage.

- **Sharpen the tool description.** The transport-vs-tool note
  currently sits inside the `timeout_sec` property description; move
  the headline ("Tool-side timeout vs transport timeout — different
  origins, different envelope shapes") into the top-level
  `description` field where it lands at every tool-pick time. Keep
  the property-level details where they are.

## 3. Invariants

- **INV-1** — `timeout_sec=N` (with minimum-floor overrides) gives a
  gate that runs under N seconds enough headroom to complete. Asserted
  by the new live test: `sleep 1` gate passes under `timeout_sec=3`.
- **INV-2** — gate `durationSec < timeout_sec` on the headroom path
  (proves the kill timer wasn't the reason the gate finished).
- **INV-3** — `gateResult.exitCode == 0` and `gateResult.passed ==
  true` on the headroom path.
- **INV-4** — the top-level `verify_changes` tool description contains
  both the literal "tool-side" and "transport" keywords, in proximity
  (within 200 chars), so Claude reads them together at pick-time.

## 4. Out-of-scope (deferred)

- The streaming-progress ping option for long builds (Vestige's
  follow-up (c)). Needs design — would change the response shape from
  one envelope to many, plus client cooperation. Separate ANTS-NNNN.
- Any change to `runOneGate`'s actual kill timing. The plumb path is
  intact; ANTS-1525 named the asymmetry; this spec just adds the
  regression-guard and bumps a description into a more visible slot.

## 5. Sources

- Vestige cross-session report 2026-05-17 Issue #1.
- ANTS-1525 (tool-side timeout envelope keys + initial asymmetry doc).
- ANTS-1492 (per-gate division fix).
- ANTS-1289 (verify_changes v1).
