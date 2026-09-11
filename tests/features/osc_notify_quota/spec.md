# Feature spec: ANTS-5033 — desktop-notification rate quota

## Problem

`TerminalGrid::handleOsc` forwards every OSC 9 body and every OSC 777
`notify;title;body` sequence straight to `m_notifyCallback`, with no
limit on how often that callback fires. When the window is unfocused
and there is no tray, `MainWindow::showDesktopNotification` turns each
callback into a detached `notify-send` process
(`QProcess::startDetached`). `cat`-ing a hostile file, or a runaway
program, spawns one process per escape sequence with no bound — from the
GUI thread.

Every OSC 9/777 sibling that reaches outside the terminal already carries
a quota: OSC 52 clipboard writes (a 60 s rolling window,
`OSC52_MAX_WRITES_PER_MIN`), the query-response amplification cap
(`QUERY_RESP_MAX_PER_SEC`), and OSC 133 forgery detection. Desktop
notifications were the one sibling with none.

## Surface

A per-terminal rolling 60 s quota, mirroring the OSC 52 write quota:
counters reset when the window advances, and once the quota is spent for
the window, the rest are dropped silently — `m_notifyCallback` is never
called for a dropped notification. The quota is **shared** across OSC 9
and OSC 777: both land in the same counter, not one each. OSC 9;4
(progress) is a distinct branch in `handleOsc` and is not gated by this
quota at all — it never calls `m_notifyCallback`.

## Invariants

- **INV-1** — An OSC 9 flood is capped. Feeding several hundred OSC 9
  notifications, each with a distinct body, on one grid yields at least
  one and at most 32 `m_notifyCallback` firings — never one per
  sequence fed.
- **INV-2** — An OSC 777 flood is capped the same way. Feeding several
  hundred `notify;title;body` OSC 777 sequences, each distinct, on a
  fresh grid yields at least one and at most 32 firings.
- **INV-3** — The quota is shared, not per-code. Alternating OSC 9 and
  OSC 777 sequences on one grid delivers no more than a flood of OSC 9
  alone delivers on a fresh grid, and at most 32. A quota per code would
  let the alternating flood through twice over, whatever the quota's
  value.
- **INV-4** — OSC 9;4 progress is not consumed by the notification
  quota. After a notification flood has exhausted the quota on a grid,
  feeding `OSC 9;4;1;42 ST` still fires the progress callback with
  state Normal and percent 42.
- **INV-5** — A full reset does not refill the quota. A flood that sends
  RIS (`ESC c`) before every notification delivers no more than an
  uninterrupted flood does, and at most 32.

## Out of scope

- The exact quota constant (this spec pins a generous upper bound, 32,
  not the decided value) — a future retune of the number is not a
  regression this test should catch.
- `MainWindow::showDesktopNotification` and the `notify-send`
  `QProcess::startDetached` call itself — this spec locks
  `TerminalGrid`'s callback rate, not the OS-level process spawn its
  caller performs per callback.

## Regression history

- **Pre-fix:** `handleOsc`'s OSC 9 and OSC 777 branches called
  `m_notifyCallback` unconditionally — no rate limit, unlike every other
  OSC branch that leaves the terminal (OSC 52, OSC 133, query
  responses). ANTS-5033.
- **Fix:** a per-terminal 60 s rolling quota shared by OSC 9 and OSC
  777, mirroring the OSC 52 write quota; excess notifications are
  dropped silently before `m_notifyCallback` is called.
