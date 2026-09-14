# Feature: review dispatcher reply deadline and size cap

## Invariants

**INV-1 — a lane has a wall-clock deadline.** `IndieReviewDispatcher`'s
`startReply` aborts a reply still running after `perLaneTimeoutMs`, in
addition to Qt's transfer (inactivity) timeout.

**INV-2 — a reply is capped.** `startReply` aborts a reply past
`LlmClient::kMaxBytes`, and the finished handler reports it as too large.

## Rationale

The ANTS-5101 performance pass found both. A transfer timeout resets on each
byte, so a trickling endpoint kept a lane open indefinitely, and replies were
read whole with no cap.

## Test surface

`test_review_dispatch_bounds.cpp` reads `src/indiereviewdispatcher.cpp`
(located from the test's own path).

## Regression history

- **ANTS-5101:** the two defects above. Locked by this spec.
