# Feature spec: ANTS-5033 / ANTS-5122 — quotas on the OSC channels that
# reach outside the terminal

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
`OSC52_MAX_WRITES_PER_MIN` and `OSC52_MAX_BYTES_PER_MIN`), OSC 1337
SetUserVar (`USERVAR_MAX_WRITES_PER_MIN`), the query-response
amplification cap (`QUERY_RESP_MAX_PER_SEC`), and OSC 133 forgery
detection. Desktop notifications were the one sibling with none.

**ANTS-5122.** A full terminal reset (RIS, `ESC c`) rebuilds the grid
and refills every one of these quotas except the notification one, which
ANTS-5033 already carries across the rebuild. `TerminalGrid::handleEsc`'s
RIS branch moves the callbacks and the notification counters into the
rebuilt grid but leaves the OSC 52 write/byte counters and the
SetUserVar write counter behind at zero. So a stream that sends RIS
before each write is never limited on those three quotas, however many
writes it sends.

## Surface

A per-terminal rolling 60 s quota, mirroring the OSC 52 write quota:
counters reset when the window advances, and once the quota is spent for
the window, the rest are dropped silently — `m_notifyCallback` is never
called for a dropped notification. The quota is **shared** across OSC 9
and OSC 777: both land in the same counter, not one each. OSC 9;4
(progress) is a distinct branch in `handleOsc` and is not gated by this
quota at all — it never calls `m_notifyCallback`.

The OSC 52 write-count quota, the OSC 52 byte quota, and the OSC 1337
SetUserVar write quota are each their own rolling 60 s window, each
independent of the notification quota above. All three, like the
notification quota, must survive a full reset: RIS changes what the
terminal displays, not how much of these channels a stream has already
spent for the window.

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
- **INV-6** — A full reset does not refill the OSC 52 write-count quota.
  A flood that sends RIS before every clipboard write delivers no more
  writes than an uninterrupted flood of the same writes does.
- **INV-7** — A full reset does not refill the OSC 52 byte quota. A
  flood of clipboard writes large enough together to spend the byte
  budget, each preceded by RIS, delivers no more bytes than an
  uninterrupted flood of the same writes does.
- **INV-8** — A full reset does not refill the OSC 1337 SetUserVar write
  quota. A flood that sends RIS before every SetUserVar write delivers
  no more writes than an uninterrupted flood of the same writes does.
- **INV-9** — Carrying these quota counters across RIS does not weaken
  RIS itself. A stream that resets before each write still ends with the
  cursor at the origin and the reset grid content, and a flood that never
  resets is still capped exactly as it always was.

## Out of scope

- The exact quota constants (this spec pins each to a generous upper
  bound or to a same-shape baseline flood measured in the same test run,
  never to the decided value) — a future retune of any of the numbers is
  not a regression this test should catch.
- `MainWindow::showDesktopNotification` and the `notify-send`
  `QProcess::startDetached` call itself — this spec locks
  `TerminalGrid`'s callback rate, not the OS-level process spawn its
  caller performs per callback.
- DEC private-mode preservation and other RIS state-reset behaviour
  already covered by `tests/features/ris_preserves_callbacks` and
  `tests/features/ris_preserves_configuration` — INV-9 reuses one of
  those assertions as a guard, it does not re-specify RIS's contract.

## Regression history

- **Pre-fix (ANTS-5033):** `handleOsc`'s OSC 9 and OSC 777 branches
  called `m_notifyCallback` unconditionally — no rate limit, unlike
  every other OSC branch that leaves the terminal (OSC 52, OSC 133,
  query responses).
- **Fix (ANTS-5033):** a per-terminal 60 s rolling quota shared by OSC 9
  and OSC 777, mirroring the OSC 52 write quota; excess notifications
  are dropped silently before `m_notifyCallback` is called. RIS carries
  the new notification counters across the rebuild, alongside the
  callbacks.
- **Pre-fix (ANTS-5122):** RIS carried the notification counters across
  the rebuild but not the OSC 52 write-count counter, the OSC 52 byte
  counter, or the OSC 1337 SetUserVar write counter — each restarted at
  zero, so a stream that sent RIS before every write on those three
  channels was never limited.
- **Fix (ANTS-5122):** RIS carries the OSC 52 and SetUserVar quota
  counters across the rebuild the same way it already carries the
  notification counters.
