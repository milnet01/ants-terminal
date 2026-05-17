# Feature spec: Current preset excludes ✅ shipped bullets (ANTS-1423)

User screenshot 2026-05-16 (during Bundle C kickoff) showed the
roadmap dialog's "Current" tab listing "8 shipped / 1 in progress /
1 planned" — but the Current preset is supposed to filter to active
work only (`ShowInProgress | ShowCurrent`, no `ShowDone`).

Root cause: the renderer's `passesFilter` logic OR's the status check
with an `isCur` rescue. `isCur` is computed from a fuzzy substring
match against `currentBullets` — which is built from the CHANGELOG's
`[Unreleased]` section plus recent commit subjects. Items the user
just shipped land in `[Unreleased]` immediately, so their ROADMAP
bullets match the current-signal and slip through the Current preset
even though their status is ✅.

## Invariants

- **INV-1 / Current preset excludes ✅ bullets even when the
  current-signal matches them.** `renderCardsHtml` with
  `filter = ShowInProgress | ShowCurrent` MUST drop any bullet
  whose status is ✅, regardless of whether `currentBullets`
  contains a fuzzy match for its body. Fix: gate `isCur` on
  `(wantDone || rec.status != "✅")`. Anchor: `ANTS-1423` in
  `src/roadmapdialog.cpp::renderCardsHtml`'s `passesFilter` lambda.
- **INV-2 / Full preset still surfaces ✅ bullets via the current
  signal.** When `wantDone` is set (e.g. Full preset), the gate is
  a no-op — current-signal rescue still applies to ✅ bullets just
  like before. Same fixture, `filter = ShowDone | ShowPlanned |
  ShowInProgress | ShowConsidered | ShowCurrent`, asserts the ✅
  bullet IS in the output.
- **INV-3 / Parallel fix in the v1 `renderHtml` path.** The
  pre-cards `renderHtml` renderer (still used by the `roadmap-query`
  IPC verb and v1 tests) has the same OR'd current-signal bug; both
  paths get the same gate so consumers downstream of either renderer
  see consistent filter semantics. Anchor: `ANTS-1423` in
  `src/roadmapdialog.cpp::renderHtml`'s `keepStatus` block.
- **INV-4 / Current-signal still surfaces non-✅ bullets in
  Current preset.** A 📋 bullet whose body matches the
  current-signal MUST still pass under Current preset — the fix
  narrows the rescue, it doesn't disable it. Same fixture, but the
  signal targets a 📋 bullet; assert the 📋 bullet IS in the
  Current-preset output.

## Test scope

Pure-function tests against `RoadmapDialog::renderCardsHtml` and
`RoadmapDialog::renderHtml` with a synthetic markdown fixture and a
hand-crafted `currentBullets` list. No GUI instantiation required.
Mirrors the `roadmap_dialog_cards` test pattern.
