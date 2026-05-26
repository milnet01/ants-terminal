# Feature: `model_switch_stats` MCP verb

Part of **ANTS-1735** (Shape B of ANTS-1226). Full design lives in
[`docs/specs/ANTS-1735.md`](../../../docs/specs/ANTS-1735.md) §2.5 (INV-13). A
read-only MCP verb that aggregates the effectiveness ledger for the caller's
project into the trust signal: *Opus turns avoided* vs *regret / under-route
rate*. Because a Max(5) subscription has no per-token bill, the scarce resources
are the Opus weekly quota and turn latency — so the headline is reported as an
avoided/regret ratio, never a flattering numerator alone.

## Scope

- **Aggregation** (`ModelSwitchLedger::statsEnvelope`,
  `ModelSwitchLedger::statsForScope`, back-compat `statsForProject`) —
  pure, read-only. Envelope:
  `{ok, switches, downgrades, upgrades, opus_turns_avoided,
  opus_turns_routed_in, regret_count, regret_rate, under_route_count,
  pending_count, by_tier, auto_model_switch_enabled, floor_tier,
  min_dwell_sec, scope, headline}`. The `_enabled`/`floor_tier`/
  `min_dwell_sec`/`scope` fields are sourced from a `StatsConfig` struct
  the dispatch layer fills from `Config::claudeAutoModel()` (ANTS-1889).
- **MCP wiring** — `RemoteControl::cmdModelSwitchStats` delegate, mainwindow
  registration, tools/list descriptor, `caller_cwd` **Required** contract,
  ETag/`fields` opt-in, optional `scope:"project"|"global"` arg (ANTS-1889).

Out of scope: writing records / filling outcomes (the controller, gated on
spikes); the gate logic (INV-1..9, `model_auto_switch`) and the ledger
storage/eviction/detection (INV-10..12, `model_switch_ledger`).

## Invariants

- **INV-13** — `model_switch_stats` is read-only (never writes ledger/config),
  returns `{ok:true, switches:0, …}` on an absent ledger, reports the headline
  as an avoided/regret ratio (when enabled with measured outcomes) or
  `"auto-switch OFF"` / `"auto-switch ON … (no switches yet)"` otherwise
  (ANTS-1889), and counts `pending` records separately from outcomes (pending
  records are never folded into regret/under-route stats).
- Stats default to the caller's project (records filtered by `project`);
  `scope:"global"` aggregates across all projects in the ledger (ANTS-1889).
- The envelope carries the live switcher config triple
  (`auto_model_switch_enabled` / `floor_tier` / `min_dwell_sec`) so callers
  can distinguish "feature OFF" from "ON, no candidates yet" (ANTS-1889).
- `caller_cwd` is **Required** (matches `roadmap_query` / `current_state`); the
  verb is registered in the tools/list descriptor with ETag opt-in.

## Method

Behavioural cases exercise `statsEnvelope`/`statsForProject` directly with
constructed records (absent ledger, mixed downgrade/upgrade/pending,
read-only-on-disk, project scoping). The dispatch wiring is locked by a
source-grep contract over `remotecontrol.{h,cpp}`, `mainwindow.cpp`, and
`claudeintegration.cpp` — the same style as `mcp_spec_query`.
