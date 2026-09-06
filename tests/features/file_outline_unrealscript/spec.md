# ANTS-4902 — `.uc` outlines as brace-family

## Background

Reported by UT_MonsterHunt, whose entire runtime surface is UnrealScript
mutators — the files most worth outlining before a read. `file_outline`
answered `language:"unknown"` with no `symbols[]`, and the reporter fell
back to `workspace_search` anchored on `^function`.

It knocks on to ANTS-4901: `workspace_search enclosing_symbol` has
nothing to attach to in a tree with no outline.

**The reporter set their own gate, and it governs this item:** ship only
if this is a table entry in the extension map; close as n/a if it needs a
bespoke parser, because UT99 modding is a narrow audience and the
workaround is cheap.

It is a table entry **plus one rule**, and the reason is worth stating
because it goes one step past the reporter's gate.

UnrealScript writes the return type between the keyword and the name:
`function bool CheckReplacement(...)`. `rxGenericDecl` does not MISS that
line — it matches it and captures `bool`. So the table entry alone would
have shipped an outline containing symbols called `bool` and `int`, which
is worse than the `unknown` it replaces: a wrong name in an outline sends
a reader to the wrong place. `rxGenericFunctionTyped` is one regex, tried
first, requiring two identifiers between `function` and the `(` — a shape
no valid JavaScript or PHP file can produce. That is not the bespoke
parser the gate was written against.

## What the generic parser covers, measured

Counted over the 190 `.uc` files in `UT_MonsterHunt/work/scratch/umsrc`:

| Declaration | Count | Outlined |
|---|---|---|
| `function` | 1190 | yes |
| `final` / `static function` | 47 | yes — both are listed modifiers |
| `simulated` / `singular function` | 4 | yes, once the UnrealScript modifiers are listed |
| `class X extends Y;` | 190 | yes |
| `struct` | 4 | yes |
| `const NAME = ...` | 30 | yes, via `rxGenericBinding` |
| `var` members | 1194 | **no** — no `=`, so no binding to match |
| `event` / `state` | 5 | **no** — see below |

`event` and `state` are deliberately not added as declaration keywords.
`event` is a real C# keyword (`event EventHandler Foo;`), so admitting it
to the shared brace-family rule would outline the TYPE as the symbol name
in every C# file. Five occurrences in 2,670 does not buy that.

`var` members are the honest gap: 1,194 of them, invisible. Navigating
to a function is what an outline is for, and the reporter asked for
exactly that, so this ships as it is rather than growing a rule.

## Invariants

### INV-1 — a `.uc` file is detected and named

`FileOutline::compute` in `Mode::Auto` reports
`language:"unrealscript"`, not `"unknown"`.

### INV-2 — classes and functions are extracted

The class declaration and every `function` spelling in the table above
appear in `symbols[]`, including the UnrealScript-only modifiers
`simulated`, `singular`, `latent`, `exec`, `native` and `iterator`.

### INV-2b — a return type is not mistaken for the function name

`function bool CheckReplacement(...)` and `static function int WaveCount()`
outline as `CheckReplacement` and `WaveCount`. `rxGenericFunctionTyped`
is tried before `rxGenericDecl` because that rule matches these lines
too; ordering is what decides the name, not which rule fires.

### INV-3 — the shared rule is not widened for the other languages

Adding those modifiers cannot change what a brace-family file in another
language outlines: a modifier is consumed only when a declaration keyword
follows it, so a line that is not a declaration still matches nothing.
