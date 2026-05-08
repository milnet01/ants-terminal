# Feature: per-style font setters disable kerning

## Problem

`TerminalWidget` builds four `QFont` variants — `m_font`,
`m_fontBold`, `m_fontItalic`, `m_fontBoldItalic` — for the four
SGR-driven render styles. The base font is constructed with
`setKerning(false)` in the constructor and re-asserted on every
variant inside `updateFontMetrics()` (called from `setFontSize` and
font-family change). This is essential for monospace correctness:
**kerning + monospace = column drift** (per project CLAUDE.md
memory note).

Pre-fix `setBoldFontFamily / setItalicFontFamily / setBoldItalicFontFamily`
(`terminalwidget.cpp:4950-4976`) reconstruct the styled `QFont`
from a user-selected family but bypass `updateFontMetrics()` and
do NOT call `setKerning(false)` themselves. Any user who configures
a custom bold/italic family in Settings → Appearance gets kerning
enabled by Qt default, drifting columns on every styled run.

Found by `/indie-review` 2026-05-08, Lane D HIGH.

## External anchor

- Qt 6 `QFont::setKerning(bool)`: https://doc.qt.io/qt-6/qfont.html#setKerning
  > "When kerning is enabled, glyph advances may include adjustments
  > based on character pairs."  → in monospace contexts these
  > adjustments break the cell-grid alignment that the renderer
  > assumes.

## Contract

### Invariant 1 — `setBoldFontFamily` calls `setKerning(false)`

Source-grep `terminalwidget.cpp` for the body of `setBoldFontFamily`:
must contain `m_fontBold.setKerning(false);`.

### Invariant 2 — `setItalicFontFamily` calls `setKerning(false)`

Same shape: must contain `m_fontItalic.setKerning(false);`.

### Invariant 3 — `setBoldItalicFontFamily` calls `setKerning(false)`

Same shape: must contain `m_fontBoldItalic.setKerning(false);`.

## How this test anchors to reality

Source-grep on `src/terminalwidget.cpp`. A runtime test would need
a custom-kerning font + glyph-advance measurement, which is fragile
and platform-dependent. The grep contract pins the same surface as
the runtime check would: each setter must explicitly disable
kerning before returning.

## Regression history

- **Pre-0.7.79:** Bold/italic styled font setters silently leaked
  Qt's default kerning-enabled state, drifting columns under
  custom-family configurations.
- **0.7.79 (ANTS-1198 from indie-review #3):** added `setKerning(false)`
  to all three setters.
