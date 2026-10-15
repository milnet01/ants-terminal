# ANTS-5719 — cold-eyes loop log

Spec: [`../specs/ANTS-5719-session-manager.md`](../specs/ANTS-5719-session-manager.md).

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|------|------|-------|----|----|----|----|---------|
| 1 | 2026-10-15 | 2 (review-lane via neutral-lane; each lane held all four questions) | 3 | 3 | 1 | 2 | Verified 9, fixed 9, dismissed 1. Q1: the tree walk missed children whose forking thread exited (now a ppid pass); session id is the basename minus `.jsonl`; `stateLabel` takes a Glyph. Q2: `NoSession` verdict; Ok may go straight to Critical; `TreeSample::approximate`. Q3: parked record owned by `SessionMonitor`, and the Parked display named. Q4: INV-4 tests a child ignoring SIGTERM through `terminateAndWait`; INV-9 scoped to the parked resume, with its own source check. Dismissed: the claim that no idle tree passes the 1000 ms/min CPU check (measured 2026-10-15 over 60 s per Claude tab tree: quiet trees 380–720 ms, others 2560–11400 ms). Unrunnable: whether MCP and clangd children exit with `claude` (needs a live session killed); the failure mode now says only that Ants does not kill them. Not measured: transcript written per message. Also corrected 190 to 191 MB (stale figure, not tallied). Both lanes found 1, 2, 3, 4, 5, 6 independently. |
