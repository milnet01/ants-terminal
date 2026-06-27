# Feature: read_region symbol-mode returns aggregate full body (ANTS-2222)

## Problem

`read_region` symbol-mode resolves a symbol's `[start, end]` range from the
flat `file_outline`, ending at the line before the *next outline entry*. For an
aggregate (`struct` / `class` / `union`) the next entry is its **first member**,
so symbol-mode returned only the declaration line — to read the whole struct you
had to fall back to line mode and hand-compute the range (DOOM_Ants feedback S4).

## Contract

- **INV-1 full body** — `read_region` symbol-mode on a named aggregate
  (`struct Foo { … };`) returns the whole body: `end_line` is the line of the
  matching closing brace, and the returned lines include every field and the
  `};`.
- **INV-2 nested braces balanced** — a method/initializer body inside the
  aggregate (its own `{ … }`) does not prematurely end the range; the close is
  the aggregate's own matching brace, not the first inner `}`.
- **INV-3 functions unaffected** — a plain function symbol still resolves to its
  own body and does not absorb a following declaration (back-compat: only
  aggregate kinds are brace-extended; `namespace` — also tagged kind `class` by
  `file_outline` — is excluded so its body isn't pulled in wholesale).

## Out of scope

- Anonymous `typedef struct { … } Name;` aggregates (a `file_outline` tagging
  question, not this slice).
- The MCP wiring/schema (unchanged — S4 is a default-behavior fix, no new arg).
