# Feature: floating prompt-jump chips (ANTS-1330)

Test contract for the two floating "jump to previous / next prompt" chips
that mirror the `Ctrl+Shift+Up`/`Down` keyboard prompt navigation with the
mouse. Design doc: `docs/specs/ANTS-1330.md` (cold-eyes-clean design; this
file owns the *test* contract, refined against the real code per the
project's testing standard).

## Why source-grep

`MainWindow` / `TerminalWidget` is too heavy to instantiate under a feature
test (PTY, tab bar, splitters). The chips are structural additions to
`TerminalWidget`, so the test asserts their *shape* in `src/terminalwidget.cpp`
via the shared `tests/_support/srcgrep.h` helpers (`slurpFunctionBody`,
`countOccurrences`, `squashWhitespace` — ANTS-2067), plus a local
`blockAround` that windows one chip's construction block. Every grep is
region-scoped, never whole-file: the literals reused from existing code
(`navigatePrompt(-1)`/`(1)` in the keyboard handler, `width() - 52` in the
scroll-to-bottom updater) would otherwise false-pass.

## Source form of the glyph

The chips set their text with the **literal UTF-8 glyph** `↑` / `↓` (not a
`\uXXXX` escape). This is a deliberate, functionally-identical deviation from
the file's escape convention (`▼` on the scroll-to-bottom chip); the test
greps the literal glyph, and the code emits it — the two are kept in lockstep
here, which is the whole point of writing the test against the real code.

## Contract

- **INV-1 — ID-scoped stylesheet reset.** Each chip calls
  `setObjectName("promptPrevBtn"|"promptNextBtn")`, and
  `styleScrollToBottomButton`'s body carries a `QPushButton#promptPrevBtn {`
  / `QPushButton#promptNextBtn {` block whose body includes the structural
  reset `padding: 0; min-width: 32px; max-width: 32px;` (ANTS-1326), so the
  app-wide `QPushButton` cascade can't inflate/clip the 32px chip.
- **INV-2 — click reuses `navigatePrompt`.** The prev chip's construction
  block contains `navigatePrompt(-1)`; the next chip's contains
  `navigatePrompt(1)` (bare `1`).
- **INV-3 — visibility gated on scrolled-up + prompts exist.**
  `updateScrollToBottomButton`'s body references
  `m_grid->promptRegions().empty()` to gate the prompt chips, and hides both
  chips in the not-shown path.
- **INV-4 — reuse-based positioning.** The updater positions the chips via
  `move(x, y - 40)` (next) and `move(x, y - 80)` (prev), reusing the
  scroll-to-bottom chip's local `x`/`y` (so the search-bar shift baked into
  `y` is inherited).
- **INV-5 — hidden at construction.** Each chip's construction block calls
  `->hide()`.
- **INV-6 — constructed exactly once.** `m_promptPrevBtn = new QPushButton`
  and `m_promptNextBtn = new QPushButton` each occur exactly once.
- **INV-7 — glyph + tooltip.** The prev block contains the `↑` glyph and a
  `setToolTip(` whose text contains "previous prompt"; the next block
  contains `↓` and a tooltip containing "next prompt".

## Manual smoke (post-relaunch)

Run a shell with OSC 133 integration, emit a few commands, scroll up → the
`↑`/`↓` chips appear above the `▼` chip; `↑` jumps to the previous prompt,
`↓` to the next; return-to-bottom hides all three; a shell without
shell-integration shows only `▼`. Open the search bar while scrolled up →
all three shift up together and none overlaps it.

## Failure surface

A refactor that drops a chip, un-wires `navigatePrompt`, loses the ID-scoped
reset (re-exposing the ANTS-1326 clip bug), forgets the `promptRegions()`
gate (dead chips on a no-integration shell), or double-constructs a chip all
fire this test.
