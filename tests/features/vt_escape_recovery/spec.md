# vt_escape_recovery — ESC-state catch-all recovery (ANTS-1998)

## Problem

The `VtParser` `Escape` state handled `[`, `]`, intermediates (0x20–0x2F),
`P`/`_`/`X`/`^`, finals (0x30–0x7E) and C0 (<0x20) — but had **no else
branch**. A byte that matched none of those (0x7F DEL, or a decoded
codepoint > 0x7E) fell through with no state transition, leaving the parser
stuck in `Escape`. The *next* byte was then mis-consumed as the ESC final,
so a single stray byte after ESC corrupted the following character.

Every other parser state (e.g. `EscapeIntermediate`) already aborts to
`Ground` on its catch-all; `Escape` was the asymmetric one.

## Fix

Add the sibling idiom `else { transition(Ground); }` to the `Escape` state.
This catches exactly the unhandled high bytes (≥ 0x7F): the malformed ESC
sequence aborts and the next byte is parsed fresh in `Ground` — matching
`EscapeIntermediate`'s existing treatment of the same bytes. The C0 branch
(<0x20, Execute-and-stay) is unchanged.

## Invariants

- **INV-1** — `ESC` `0x7F` `M`: the ESC aborts on DEL; `M` is printed as
  text (no `EscDispatch`).
- **INV-2** — a well-formed escape (`ESC` `M` = RI) still dispatches:
  the catch-all does not regress the normal final-byte path.
