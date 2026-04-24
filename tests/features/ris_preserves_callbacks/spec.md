# Feature: RIS (ESC c) preserves integration callbacks

## Contract

When the VT command `ESC c` (RIS — Reset to Initial State) fires,
`TerminalGrid` must wipe visible terminal state (cells, cursor, modes,
scrollback, attrs, SGR, DEC private modes, tab stops, alt-screen
selection) but must NOT wipe any callback installed by the embedder.

The following integration-layer handles MUST survive a RIS:

1. `m_responseCallback` — DA/CPR/DSR report sink (already preserved
   prior to 0.7.17; kept as an invariant anchor).
2. `m_bellCallback` — BEL audible/visual bell.
3. `m_notifyCallback` — OSC 9 / OSC 777 desktop notifications.
4. `m_lineCompletionCallback` — trigger-system grid-mutation hook fired
   at line completion.
5. `m_progressCallback` — OSC 9;4 progress reports.
6. `m_commandFinishedCallback` — OSC 133 D exit-code notifications.
7. `m_userVarCallback` — OSC 1337 SetUserVar reports.
8. `m_osc133ForgeryCallback` — OSC 133 HMAC-failure alarm.

Additionally `m_osc133Key` (the HMAC secret read from
`$ANTS_OSC133_KEY` at process start) MUST survive, since it is a
process-level security configuration independent of grid state.

## Rationale

Before 0.7.17 the RIS handler at `terminalgrid.cpp handleEsc case 'c'`
stashed only `m_responseCallback` and `m_bellCallback`, then
re-assigned `*this = TerminalGrid(rows, cols)`, which move-constructs
a fresh grid whose callback members are default (empty) functions.
Any other callback member therefore became an empty `std::function`
after a single `tput reset` / `reset(1)` / `stty sane` — silently
breaking:

- Desktop notifications emitted by running commands
  (`osc9` / `notify-send`-via-terminal).
- `command_finished` plugin event (trigger-system, audit dialog
  command-row mapping).
- Progress bars reported by long-running commands (rsync, apt).
- SetUserVar theming hooks (jj, starship).
- OSC 133 HMAC forgery alarm (security-critical — a well-timed
  `tput reset` silences the alarm permanently).

Because the issue manifests only after a RIS plus a subsequent
trigger of the relevant hook, it is easy to miss in manual testing.
This feature test fires RIS then re-triggers every hook and asserts
each callback fires a second time.

## Invariants

**INV-1 — Pre-RIS baseline.** All 8 callbacks fire exactly once when
their triggering escape is fed to a fresh grid.

**INV-2 — Post-RIS all callbacks fire again.** After `\x1b c` wipes
state, the same 8 triggers fire the same 8 callbacks. Counter
increments from 1 → 2 on each.

**INV-3 — Grid state reset.** After RIS, `cursorRow() == 0` and
`cursorCol() == 0`, and a pre-RIS written cell is cleared. Confirms
RIS still does its actual job — we're preserving callbacks, not
preserving state.

**INV-4 — `m_osc133Key` survives.** With a non-empty key installed
via `setOsc133KeyForTest` pre-RIS, the `osc133HmacEnforced()` accessor
still reports `true` post-RIS. Confirms the HMAC enforcement mode is
not silently disabled by a reset.

**INV-5 — Forgery callback fires post-RIS.** With key installed, an
unsigned `ESC ] 133 ; A BEL` sequence fired post-RIS increments
`osc133ForgeryCount()` and calls the forgery callback.

## Scope

### In scope
- Every callback member of `TerminalGrid` listed above.
- `m_osc133Key` preservation (security configuration).
- A smoke check that grid state (cells, cursor) is actually reset.

### Out of scope
- DEC private-mode preservation. DEC modes are **state**, and RIS
  resetting them is part of the RIS contract — not a regression.
- Alt-screen state preservation. Ditto.
- Scrollback preservation. Ditto.
- Non-RIS resets (DECSTR / soft reset, which we do not handle as RIS).

## Regression history

- **Pre-0.7.17:** Only `m_responseCallback` + `m_bellCallback`
  preserved. 6 other callbacks silently wiped on every RIS. No
  regression test; the drop was noticed in the 2026-04-23 re-review
  sweep and fixed in 0.7.17.
- **0.7.17:** All 8 callbacks + `m_osc133Key` preserved. This test
  locks the contract so any future refactor of the RIS handler has
  to re-account for every callback or break a visible invariant.
