# Feature: file_outline registers typedef-struct aggregates (ANTS-2228)

## Problem

file_outline's cpp scanner caught a bare `struct X;` forward decl but skipped
the `typedef struct NAME_s { … } NAME_t;` definition — the dominant C aggregate
idiom — so read_region symbol-mode (and ANTS-2222's aggregate-body slice) could
not reach any engine struct on a C header (DOOM_Ants feedback, r_defs.h).

## Contract

- **TS-1 tagged typedef** — `typedef struct vertex_s { … } vertex_t;` registers
  TWO aggregate symbols at the opening line: one keyed by the alias
  (`vertex_t`) and one by the tag (`vertex_s`), each kind `class` with a
  signature beginning `struct`.
- **TS-2 anonymous typedef** — `typedef struct { … } anon_t;` registers one
  aggregate symbol keyed by the alias (`anon_t`).
- **TS-3 aggregate-body read** — `read_region` symbol-mode on the alias OR the
  tag returns the FULL struct body (opening line through the matching
  `} ALIAS;`), via the ANTS-2222 brace-match (the signature begins `struct`).
- **TS-4 nested braces balanced** — an inner anonymous `struct { … } field;`
  does not end the outer typedef early; the close is the outer matching brace.
- **TS-5 forward decl unaffected** — a bare `struct line_s;` forward decl is
  still registered as before (back-compat), and a bodyless
  `typedef struct foo_s foo_t;` (no `{`) registers nothing new.

## Out of scope

- `typedef struct TAG { … }` whose `{` sits on the NEXT line (the dominant
  idiom puts it on the `typedef struct TAG {` line) — line mode covers it.
- Multiple declarators on the close line (`} a_t, *a_p;`) register the first
  alias only.
