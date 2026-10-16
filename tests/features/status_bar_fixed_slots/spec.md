# Feature: ANTS-5753 — fixed status-bar slots

User request 2026-10-16: the right-hand status-bar pills move as their
text changes or as they show and hide, which makes them hard to read.
Each pill keeps a fixed slot wide enough for its longest text.

## Invariants

- **INV-1** — `statusbarslot::pin(w, candidates)` fixes `w`'s width to the
  widest `sizeHint()` among the candidate texts in its current style, leaves
  `w`'s own text as it was, and sets retain-size-when-hidden. Works for a
  `QLabel` and a `QAbstractButton`. Breaks when: the width follows the
  current text, the text is left as a candidate, or a hidden widget gives
  its slot up.
- **INV-2** — `statusbarslot::setRetain(w, false)` gives a hidden widget's
  slot back. A pill the user switched off in Settings uses it, so a disabled
  feature takes no space. Breaks when: the flag does not change.
- **INV-3** — `ClaudeStatusBarController::pinSlots()` pins every
  variable-text Claude pill (`m_statusLabel`, `m_tokensSavedChip`,
  `m_mailChip`, `m_reviewBtn`, `m_bgTasksBtn`, `m_tasksBtn`, `m_modelBtn`,
  `m_modelStateBtn`, `m_undoSwitchBtn`, `m_errorLabel`) and keeps
  `m_contextBar`'s slot when hidden; `applyTheme` calls it. Breaks when: a
  pill is dropped from the list, or a theme change no longer re-measures.
- **INV-4** — a Claude state text wider than its slot (a raw tool name) is
  elided to the slot, with the full text in the tooltip. Breaks when:
  `apply()` sets the raw text unelided.
- **INV-5** — the Roadmap button keeps its slot when hidden, and the
  "Add to allowlist" prompt is inserted at the left end of the permanent
  group (`insertPermanentWidget(0, …)`) so the pills do not shift when it
  appears. Breaks when: either is appended with `addPermanentWidget`.

Tests: `test_status_bar_fixed_slots.cpp` — INV-1 and INV-2 behaviourally
on real widgets; INV-3 to INV-5 by source scrape.
