# ANTS-1579 — `verify_changes` `timeout_sec` end-to-end plumb

See `docs/specs/ANTS-1579.md`. This spec.md mirrors the invariants the
test enforces.

## Contract

A caller-supplied `timeout_sec` actually reaches `runOneGate`'s
per-gate budget. A gate that runs under the supplied budget completes
naturally — it is NOT killed by the kill timer. Pairs with the
existing `verify_changes_engine.Inv2TimeoutKillsHangingGate` for
bidirectional plumb coverage.

## Invariants

- **INV-1** — A 1 s `sleep 1` gate under `opts.timeoutSec=3` runs to
  completion (not killed).
- **INV-2** — The gate's `durationSec < timeout_sec` on the headroom
  path (proves natural completion, not kill-on-expiry).
- **INV-3** — `gateResult.exitCode == 0` and `gateResult.passed ==
  true` on the headroom path.
- **INV-4** — The `verify_changes` tool description names both the
  literal "tool-side" and "transport" keywords in proximity (within
  300 chars) so a Claude reader sees the asymmetry up front. (Source-
  scrape.)
