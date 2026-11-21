<!-- ants-spec-format: 1 -->
# ANTS-3464 — Output redraws only the rows it changed

**Status:** implemented (2026-11-21). No review gate: not a contract, cheap to reverse (rule 14).
**Kind:** perf.
**Source:** ROADMAP.md ANTS-3464 (in-session-2026-07-09).
**Pairs with:** ANTS-3454, ANTS-3451.

**Layman:** When a program changes one line — the prompt you type at, a
spinner — the terminal redraws that line instead of the whole window.

## 1. Problem

After every VT batch, `TerminalWidget::onVtBatch` calls
`TerminalWidget::scheduleCoalescedUpdate`, which calls a full `update()`,
and so does the `m_paintPacer` timeout. `paintEvent` has honoured
`event->rect()` since ANTS-3454, so a full `update()` repaints every row.

1. A full paint of a 52×210 grid costs 4.9 ms (plain), 6.6 ms (styled) and
   11.3 ms (CJK) per frame (`./build/bench_full_paint`, 2026-11-21). The
   frame budget at 60 fps is 16.7 ms.
2. Typing at a prompt, or a spinner rewriting its line, changes one or two
   rows per batch.
3. `TerminalWidget::onGridLineCompleted` calls a full `update()` of its own
   on each completed line whenever trigger rules are set. That repaint
   bypasses the pacer.

The grid's per-row dirty bits (`TerminalGrid::screenLineDirty`) cannot
drive the repaint on their own:

- `paintEvent` clears every bit (`invalidateSpanCaches` →
  `clearAllScreenDirty`), even on a cursor-blink paint of one cell.
- Some changes set no bit: cursor moves, cursor visibility (DECTCEM) and
  shape (DECSCUSR), inline image placement and removal.
- The bits index screen rows. They match viewport rows only while
  `m_scrollOffset == 0`.

## 2. Surface

### 2.1 Damage harvested per batch

`TerminalWidget` gains `void harvestOutputDamage()`, called in
`onVtBatch`'s not-in-sync branch just before `scheduleCoalescedUpdate()`.
It reads the dirty bits without clearing them and unions the result into
`QRegion m_outputDamage`, or sets `bool m_outputDamageFull`.

The harvest adds a full-width band, `QRect(0, m_padding + r *
m_cellHeight, width(), m_cellHeight)`, for:

- each screen row `r` whose dirty bit is set;
- the cursor's row as last harvested and its row now
  (`effectiveCursorRow`), each widened 4 px up and down for the glow
  `blinkCursor` already covers;
- the rows of the bracket pair the last paint drew (recorded by
  `paintEvent`) and of the pair `findMatchingBracket` returns now.

It sets `m_outputDamageFull` instead when any of these holds:

- **F1** `m_scrollOffset != 0`.
- **F2** a sync-output snapshot is held (`m_frozenScreenRows` non-empty),
  or this batch ended a sync block.
- **F3** `scrollbackPushed()` moved during the batch.
- **F4** every screen row is dirty.
- **F5** `cursorVisible()` or `cursorShape()` differs from the last harvest.
- **F6** `inlineImages()` is non-empty, or its size differs from the last
  harvest.
- **F7** `promptRegions()` changed since the last harvest: its size, or the
  last region's `id`, `endLine`, `commandEndMs`, `exitCode`, `hasOutput` or
  `outputStartLine`.
- **F8** `m_showPerfOverlay` or `m_urlQuickSelectActive`.
- **F9** the batch's `clearSelectionHint` cleared a selection.
  `clearSelection` requests no repaint of its own.

### 2.2 Issuing it

`scheduleCoalescedUpdate` and the pacer timeout each issue the harvested
damage in place of `update()`: `update()` when `m_outputDamageFull`,
otherwise `update(m_outputDamage)`. Both are then cleared. Nothing else
clears them, so damage harvested while the pacer waits survives any paint
in between.

### 2.3 `onGridLineCompleted`

Its trailing `update()` goes. It runs only inside `onVtBatch`'s
`processAction` loop, through the grid's line-completion callback, and the
rows it changes (`applyRowAttrs`, `addRowHyperlink`) set their dirty bits,
so the batch's harvest covers them.

### 2.4 Test seam

`QRegion takeOutputDamageForTest()` first issues what is pending, as the
pacer's next tick would, then returns the union of every region issued
since the last call, with a full update as `rect()`. So each call is one
frame's damage: a test runs no event loop, and the pacer never fires.

## 3. Invariants

- **INV-1** — A picture built only from the issued damage matches a full
  paint, pixel for pixel, after every batch. *Test:*
  `OutputDamageRows.Inv1PartialPaintsMatchAFullPaint` replays scenarios
  (typing with backspace, cursor moves with no writes, a spinner rewriting
  the last row and a row above it, wide and combining text, bracket
  matching, a trigger rule highlighting a completed line). After each feed
  it paints only `takeOutputDamageForTest()` onto a kept canvas and
  compares the canvas with a fresh full paint. *Breaks when:* a change
  sets no dirty bit and no fallback covers it.
- **INV-2** — One typed character at an unscrolled prompt issues damage no
  taller than the cursor row's band. *Test:*
  `OutputDamageRows.Inv2OneCharacterDamagesOneRow`. *Breaks when:* the
  harvest falls back to full, or adds rows nothing changed.
- **INV-3** — Each of F1 to F9 issues a full update. *Test:*
  `OutputDamageRows.Inv3FallbacksRepaintEverything`, one case per
  condition. *Breaks when:* a fallback check is dropped or reads the wrong
  state.
- **INV-4** — Damage pending behind the pacer survives a paint that clears
  the dirty bits. *Test:* `OutputDamageRows.Inv4PendingDamageSurvivesABlinkPaint`
  feeds two batches inside one pacer window, paints a cursor-cell rect in
  between, and checks the second batch's rows are still pending.
  *Breaks when:* the damage is rebuilt from the dirty bits at issue time.

## 4. Out of scope

- Scrolling output stays a full repaint (F3). Narrowing it needs a pixel
  scroll of the backing store, a separate design.
- User-interaction repaints (selection, search, wheel scroll) keep their
  full `update()`.

## 5. Tests

Feature test `tests/features/output_damage_rows/`, covering INV-1..4,
label `features;fast`. INV-2 and INV-4 fail against the pre-change
source, which repainted everything. INV-1 and INV-3 pass there by
construction, so each is shown to fail against a deliberately broken
harvest instead: one that drops a fallback, the old cursor row, the
dirty rows or a bracket row.

## 6. Reload

Paint code runs in the terminal process, so it reaches a running terminal
only on restart. It holds no state that outlives a frame, so it has no
stop/start control.

## 7. Cross-doc impact

CHANGELOG `[Unreleased]`.

## Cold-eyes loop log

No review ran: not a contract, cheap to reverse (rule 14).
