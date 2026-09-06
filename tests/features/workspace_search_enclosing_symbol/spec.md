# workspace_search enclosing_symbol — ANTS-2220

Opt-in `enclosing_symbol:true` on `workspace_search` annotates each match
with `enclosing:"Foo::bar"` — the function/method the matched line lives
inside — folding the usual "which function is this in?" follow-up
(`file_outline` after a search) into the search itself, the way
`find_definition include_body` folded in the post-find read.

Spec of record for the verb: `docs/specs/ANTS-1248.md`. This test pins the
ANTS-2220 slice: the pure `RemoteControl::enclosingSymbolForLine` heuristic
(runtime) plus the handler + schema wiring (source-grep — the handler needs a
live `MainWindow` to invoke).

## Invariants

| # | Statement |
|---|-----------|
| ES-1 | `enclosingSymbolForLine(symbols, line)` returns the name of the symbol whose start line is the greatest `≤ line`. A match inside a function resolves to that function. |
| ES-2 | Nearest-preceding semantics: a line in the GAP between two top-level symbols (after symbol A's start, before symbol B's start) attributes to A. The flat outline carries start lines only — gap attribution to the preceding symbol is the documented heuristic. |
| ES-3 | A `line` before the first symbol's start (e.g. a match up in the includes) returns an empty string — no `enclosing` is emitted for it. |
| ES-4 | The exact start line of a symbol resolves to that symbol (`start ≤ line` is inclusive). |
| ES-5 | A qualified method name (`Widget::compute`) is returned verbatim, so the annotation reads as `Foo::bar`. |
| ES-6 | An empty `symbols` array returns an empty string (no crash on a file the outline could not scan). |
| WI-1 | `cmdWorkspaceSearch` in `remotecontrol.cpp` reads `enclosing_symbol`, scans each distinct matched file via `FileOutline::compute`, resolves each match through `enclosingSymbolForLine`, and writes the `enclosing` field. |
| WI-2 | `RemoteControl::enclosingSymbolForLine` is declared as a `static inline` helper in `remotecontrol.h` (testable without the MainWindow chain). |
| WI-3 | The MCP `tools/list` schema in `claudeintegration.cpp` registers the `enclosing_symbol` property on `workspace_search`. |

## Acceptance

Exit 0 = all invariants hold. ES-1..ES-6 call the pure helper directly (no Qt
event loop); WI-1..WI-3 are source-greps via the `SRC_*` compile defs the
`test_claude` bundle already supplies.

## Out of scope

- Brace-bounded (precise) enclosing attribution — would need per-symbol end
  lines, which the flat outline does not carry. Nearest-preceding is the
  documented, cheap heuristic (ES-2).
- Per-`also_at` duplicate annotation — only the primary match in a dedup group
  carries `enclosing`; `also_at` entries stay bare `{file, line}`.
- Runtime exercise of the full handler — it needs a live `MainWindow`; the
  per-file scan + cache loop is locked by WI-1 source-grep.

## ANTS-4901 — the annotation says what it managed

Reported by UT_MonsterHunt. `enclosing_symbol:true` over a tree of files
whose language has no outline returned every row without `enclosing` and
said nothing about why. The reporter learned the reason by calling
`file_outline` separately — the round-trip the annotation exists to
remove.

**The ambiguity is the defect, not the absence.** ES-3 makes "a match
above the first symbol carries no `enclosing`" a legitimate empty case,
so a reader cannot tell that from "this language is not outlined at all"
— and the second means no row in that file can ever be annotated. Same
class as `spec_lint`'s `sections_checked:false`: a caller who cannot
tell "checked, nothing there" from "never ran" reads the second as the
first.

### INV-1 — the envelope reports the outcome

An annotated search carries `enclosing_annotated` (rows that got a
symbol) and, when non-empty, `enclosing_symbol_unavailable` — the files
whose language yielded no outline, capped at 20 with the true total in
`enclosing_symbol_unavailable_count`.

### INV-2 — unavailable is about the LANGUAGE, not the symbol count

A file that outlines but happens to declare nothing, or one where a
particular match sits above the first symbol, is ES-3 and is NOT listed.
The two facts stay distinguishable, which is the whole point.

### INV-3 — the keys ride the opt-in

A search that did not pass `enclosing_symbol` returns the envelope it
returned before, byte for byte.
