# Feature: model-switch effectiveness ledger (`ModelSwitchLedger`)

Part of **ANTS-1735** (Shape B of ANTS-1226). Full design + invariants live in
[`docs/specs/ANTS-1735.md`](../../../docs/specs/ANTS-1735.md) §2.5. Because the
user does not click the recommender chip, **measured outcomes replace
click-acceptance** as the trust signal. Every auto-switch appends one record; a
later tick fills its outcome once the following turns have run.

## Scope

`src/modelswitchledger.{h,cpp}` (pure Qt6 Core/JSON, in `ants_core_lib` so the
read-only `model_switch_stats` MCP verb can reach it):

- **append + byte-cap eviction with pending-pinning** (`appendRecord`,
  `writeRecords`, `evictToCap`).
- **JSON (de)serialization** of the record (field names match §2.5 verbatim).
- **outcome detection** — `detectUserOverride`, `detectUnderRoute`,
  `detectCorrection` (pure; the controller feeds live transcript data).

Out of scope: the live controller that writes records and fills outcomes
(actuator wiring, gated on spikes), and the stats aggregation surfaced by
`model_switch_stats` (INV-13, tested under `mcp_model_switch_stats`).

## Invariants

- **INV-10** — the ledger file is mode 0600 and never exceeds 256 KiB; eviction
  drops whole oldest lines and never a pending-outcome record nor a partial
  line. `kMaxLedgerBytes == 256 * 1024`.
- **INV-11** — `user_override_within_5_turns` is true iff an in-window transcript
  `/model X` has **no** auto ledger record matching by `to_tier == X` and
  `|Δts| ≤ kAuthorWindowMs` (10 s) — so the controller's own injections (and a
  second auto-switch in the same window) never count as overrides.
- **INV-12** — `under_route_signal_within_5_turns` is true iff, within 5 turns of
  a downgrade, a higher tier is re-recommended; a downgrade with zero following
  turns stays `Pending` and is never counted as not-under-routed.

Soft signal (not a numbered invariant): the correction regex
`\b(no|wrong|that's not|undo|revert|try again)\b` (case-insensitive, linear /
no ReDoS) fires on the listed cues — including the documented prose
false-positive ("no problem") — and is treated as a soft indicator only.

## Method

A `QTemporaryDir` holds the ledger file. Eviction tests use a small `capBytes`
to exercise the drop-oldest logic cheaply, plus a constant check that the
production cap is 256 KiB. Detection functions are exercised as pure
value-in/value-out — the override/under-route/correction cases each pair a
positive (signal fires) with a negative (no false fire).
