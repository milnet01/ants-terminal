# Feature: SGR attribute reset invariants

## Contract

For every SGR "set" code that toggles a cell attribute ON, there exists
a corresponding "reset" code that toggles it OFF. Applying the set
code, then the reset code, then writing a cell, must produce a cell
with the attribute **cleared**. The set/reset pairs are:

| Set | Resets | Attribute |
|---|---|---|
| 1 / 2 | 22 | bold / dim |
| 3 | 23 | italic |
| 4 / 4:x (x>0) | 24 / 4:0 | underline (any style) |
| 7 | 27 | inverse |
| 9 | 29 | strikethrough |
| 53 | 55 | overline (no-op in our impl, but set must not persist) |

Additionally, SGR 0 (full reset) must clear every attribute in a
single code.

## Rationale

Claude Code v2.1+ sets SGR 9 (strikethrough) on completed task-list
items. Without a reliable 29 reset path, the attribute leaks onto
subsequent cells — pending items render struck-through, blank rows
render with horizontal mid-cell lines. The user-reported artifact
("even ▶ pending tasks show strikethrough, and blank rows have
horizontal dashes") boils down to: attribute set is honored, attribute
reset is not.

This spec asserts the set→reset→write round-trip produces a clean
cell, for every pair. If the renderer also applies a render-side
defensive filter (e.g. don't draw strikethrough on space cells), that
filter is cosmetic; this test still requires the attribute itself to
be correctly cleared.

## Invariant

For each (setCode, resetCode) pair:

1. Emit `ESC [ <setCode> m`.
2. Write one printable character; confirm the cell carries the
   attribute.
3. Emit `ESC [ <resetCode> m`.
4. Write one more printable character; the new cell's attribute must
   be **cleared**.

And for SGR 0:

1. Emit `ESC [ 1;3;4;7;9 m` (set bold+italic+underline+inverse+strike).
2. Write a character; confirm all five attrs set.
3. Emit `ESC [ 0 m`.
4. Write another character; all five attrs must be cleared.

## Scope

### In scope
- The bold, dim, italic, underline, inverse, strikethrough set/reset
  pairs.
- SGR 0 full-reset.
- Mixed set sequences (`ESC [ 1;4m` setting both at once) followed by
  targeted reset (`ESC [ 24 m` — underline only — leaves bold on).

### Out of scope
- Overline (no cell-attribute field in our impl; the SGR codes are
  acknowledged as no-ops, so no leak is possible).
- Conceal (same — no cell-attribute field).
- Underline style sub-params (4:3 curly, 4:4 dotted, etc.) — covered
  by a separate test if it ever becomes a regression candidate.
- Color attributes (fg/bg) — different reset model (SGR 39 / 49); own
  test.

## Regression history

- **0.6.22** — user-reported visual artifacts under Claude Code TUI:
  pending task-list items rendered with strikethrough; blank rows
  rendered with horizontal mid-cell dashes. Diagnosis: the *render
  side* drew strikethrough on space cells carrying the attribute,
  making decorative bleed visually obvious. The fix was twofold:
  (a) render-side filter skips strikethrough/underline on empty-glyph
  cells, (b) this test asserts the *attribute* itself is correctly
  cleared regardless of render filter. Prevents regression of either
  half.
