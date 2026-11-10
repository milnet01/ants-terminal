# Feature spec: ANTS-5075 — a large paste reaches the shell whole

A paste used to reach the PTY as three queued writes: the bracketed-paste start
marker, the body, and the end marker. `Pty::write` drops any write that would
take its pending queue past `MAX_PENDING_WRITE_BYTES`, so a large paste lost
its tail, its end marker, or both. A lost end marker leaves the shell in
bracketed-paste mode. The only signal was a `qWarning` in `VtStream`.

The fix feeds the paste to the PTY in slices as the write queue drains
(user decision, 2026-09-14).

## Invariants under test

- **INV-1 — the whole paste arrives.** A payload larger than
  `MAX_PENDING_WRITE_BYTES`, handed to `VtStream::writePaste`, reaches a child
  that reads its terminal in raw mode byte for byte, end marker last.
  Behavioural: a real `VtStream` and child process.
- **INV-2 — no write is lost on the way.** `Pty::writeLost` does not fire
  while that paste is fed. Behavioural.
- **INV-3 — keys typed during a paste land after it.** Bytes passed to
  `VtStream::write` while a paste is still being fed arrive after the paste's
  last byte, never inside it. Behavioural.
- **INV-4 — the widget sends a paste as one call.** `TerminalWidget::performPaste`
  hands the wrapped payload to `VtStream::writePaste` in a single invoke, not
  as separate marker and body writes. Source scrape.
- **INV-5 — what queues behind a stalled paste is capped (ANTS-5415).** A
  write, or a second paste, arriving while a paste is fed is queued only if
  the bytes queued behind that paste stay within `Pty::MAX_PENDING_WRITE_BYTES`.
  Past that it is dropped and logged ("queued behind a stalled paste"); a
  smaller write after it still lands. The paste itself is never capped.
  Behavioural: the child sleeps before reading, so the paste stalls.

## Red proof

Before the fix `VtStream::writePaste` is a stub that forwards to `Pty::write`,
so INV-1 and INV-2 fail on the dropped tail; INV-3 fails because the typed
bytes are written as soon as they arrive; INV-4 fails on the scrape.
