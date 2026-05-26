# Feature: ANTS-1735 outcome fill-in tick wiring

The §2.5 outcome fill-in
(`ClaudeStatusBarController::fillPendingLedgerOutcomes`) reads the global
ledger, calls `ModelSwitchLedger::computeOutcome` per pending record, and
writes back when anything changed. The pure `computeOutcome` helper is
exercised by `tests/features/model_switch_ledger/`; this test locks the
controller-side wiring against `src/claudestatuswidgets.cpp` and
`src/mainwindow.cpp`.

## Wiring contract

1. **Throttled** — the method bails when `now - m_lastPendingFillMs <
   kPendingFillIntervalMs` (30 s). The 2 s status timer fires it ~15× per
   minute; only one of those does work.
2. **Cheap when nothing to do** — early-return when the ledger has no
   records or no record is `pending`.
3. **Uses the pure helper** — calls
   `ModelSwitchLedger::computeOutcome(...)` per pending record, then
   `writeRecords` if any record changed.
4. **Wired to the 2 s status timer** — `mainwindow.cpp` `connect`s
   `m_statusTimer::timeout` to `fillPendingLedgerOutcomes` alongside
   `refreshAutoModelSwitch`.

Source-grep test — the behavioural surface (turns-on-to-tier counting,
under-route detection, settlement) is covered by `ModelSwitchLedger`
gtests; what remains is to lock the controller-level wiring against
silent removal.
