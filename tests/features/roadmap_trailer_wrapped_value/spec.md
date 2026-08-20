# ANTS-4542 — a hard-wrapped trailer value is not truncated at the wrap

Status: implemented

## Problem

Every trailer value pattern captures with `([^\.\n]+?)\s*[\.\n]` — the
stop class contains `\n`, so a value that is hard-wrapped mid-phrase ends
at the wrap and the remainder is lost.

Measured on this project's `ROADMAP.md`, 2026-08-20: of 2199 bullets, 123
state a trailer key both inline (as pre-migration prose) and as a rendered
line-initial trailer, and the divergent ones are the wrapped values.

    Kind: implement. Lanes: MainWindow,
    TerminalWidget.

parses `lanes: ["MainWindow"]`. `TerminalWidget` is dropped, and because
the renderer then emits the stored column terminated with a full stop, the
loss renders as a well-formed `Lanes: MainWindow.` — indistinguishable
from a bullet that really declares one lane.

The same shape truncates mid-parenthetical, which is what makes it
visible at all: four values in this project render with an unbalanced
open bracket, e.g. `Source: in-session-2026-05-21 (noticed integrating.`
and `Lanes: new (hooks/.`

Reported independently as ANTS-4553 (finbreak: a bullet whose prose lists
four lanes renders a stored list of three beside it) and as ANTS-4542's
half (b).

This is import-time damage, not ongoing corruption: the renderer emits
body text verbatim and never re-wraps it (`roadmaprender.cpp:443`), and
it always emits a trailer on its own line terminated by a full stop. So
the rule below is a no-op on generated content and changes only how
legacy hand-wrapped prose is read.

## Surface

- `src/roadmapparse.cpp` — `matchLastIn()`, shared by all five keys.

## Rule

A trailer value whose match ends at a line break — rather than at a
sentence-terminating full stop — continues onto the following line. It
stops at the first full stop, at a blank line, at a line that begins a new
trailer declaration, or after four continuation lines.

Continuation is decided against the code-span mask, so a key inside
backticks on the next line does not end the value; the text is sliced from
the unmasked body, so a value legitimately carrying backticks is stored
verbatim. Both halves match `matchLastIn()`'s existing contract.

Lines are joined with a single space, which is the character the wrap
replaced.

## Invariants

- **INV-1** — `Lanes: build, ci, tests,` wrapping onto `security.` yields
  four lanes, not three.
- **INV-2** — A value wrapped mid-parenthetical is rejoined whole:
  `Source: user-2026-04-30 (two reports, same` + `day).` yields
  `user-2026-04-30 (two reports, same day)`.
- **INV-3** — A value already terminated by a full stop does NOT absorb
  the line below it.
- **INV-4** — A continued value stops at its own full stop and does not
  swallow a trailer that follows on the continuation line: `Lanes: a, b,`
  + `c. Kind: chore.` yields three lanes AND `kind: chore`.
- **INV-5** — A full stop inside a token does not terminate a value: a
  `Source:` naming a `.md` file keeps the whole filename.
- **INV-6** — A blank line ends the value; a trailer never continues
  across a paragraph break.
- **INV-7** — Regression: the unwrapped forms ANTS-2058 locked still
  parse — inline `Kind: X. Lanes: Y.` on one line, and a line-initial
  `Lanes:` on its own line.

## Tests

`test_roadmap_trailer_wrapped_value.cpp` drives the pure static
`RoadmapDialog::parseBullets` and asserts the parsed `kind` / `lanes` /
`source` fields.
