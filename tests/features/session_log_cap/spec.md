# Feature: each tab's session log stops at 1 GB

## Problem

With Session Logging on, every tab writes the terminal's raw output to
`$XDG_DATA_HOME/ants-terminal/logs/session_<stamp>.log` with no size
limit (ANTS-6493). A tab that prints without end fills the drive.

## Decision

The user decided on 2026-11-17: cap each tab's log at 1 GB. At the cap
the tab stops logging and a message says the log is full. What is
already saved stays. No rotation.

## Invariants

- **INV-1** `TerminalWidget::sessionLogRoom(written, incoming, cap)`
  returns how many of `incoming` bytes fit in a log already holding
  `written`: all of them below the cap, the remainder at the boundary,
  and 0 once the log is full.
- **INV-2** `TerminalWidget::kSessionLogCapBytes` is 1 GB
  (1,000,000,000 bytes).
- **INV-3** `TerminalWidget::onVtBatch` writes only the bytes
  `sessionLogRoom` allows. When a batch does not fit, it closes the
  log, turns logging off for that tab, and emits `captureFailed` with
  a message saying the log is full. `captureFailed` reaches the status
  bar through `MainWindow`.
- **INV-4** `TerminalWidget::setSessionLogging` resets the byte count
  when it opens a new log, so turning logging back on starts a fresh
  1 GB log.

## Tests

`test_session_log_cap.cpp`: INV-1 and INV-2 call the helper; INV-3 and
INV-4 read the function bodies, since a 1 GB write cannot run in a
test.
