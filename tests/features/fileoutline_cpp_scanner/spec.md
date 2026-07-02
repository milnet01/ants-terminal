# Feature: file_outline C++ scanner scope-awareness (ANTS-2159)

Test contract for the two coupled defects DOOM reported in the
single-line `rxCppFunc` scanner and reproduced on Ants' own source:

- **(a) FALSE POSITIVE** — `Type name(expr);` most-vexing-parse locals and
  statement lines after a `case` label (`case X: return f();`) — both
  *inside a function body* — were tagged `kind:func`. Root cause: the
  scanner had no scope awareness (it processes each trimmed line in
  isolation; a local declaration is syntactically identical to a
  file-scope function declaration — only the brace scope tells them apart).
- **(b) FALSE NEGATIVE** — id-Software / GNU style `void\nName(args)\n{`
  with the return type on the previous line (and/or the body brace on the
  next line) never matched the same-line-only regex.

The fix: track brace scope (a string/char/comment-aware brace counter +
`funcBodyDepth`) so function/member symbols are emitted only at file or
type-body scope, never inside a code body; and recognise a `Name(args)`
definition whose return type and/or `{` sit on adjacent lines.

## Invariants

- **INV-1** — a local `Type name(arg);` inside a function body is not a
  func symbol; the enclosing function is.
- **INV-2** — `case X: return helper(...)` inside a body is not a func.
- **INV-3** — `void\nName(args)\n{` (return type on the previous line) is
  detected.
- **INV-4** — brace-on-next-line def (`int beta(int)\n{`) is detected, and
  a local inside it is not.
- **INV-5** — positive controls still work: same-line free function,
  qualified member definition (`Foo::bar`), class-member declaration.
- **INV-6** — a file-scope prototype (`int proto(int a);`) is detected and
  does NOT open a body (the next file-scope line is still scanned).
- **INV-8** (ANTS-3351) — an `extern "C"` (or `extern "C++"`) linkage
  specifier on a function definition does not hide it: the function is
  detected AND its body's most-vexing-parse locals are suppressed. The
  `"C"` string literal previously broke the return-type match, so every
  `extern "C"` definition went undetected and its interior locals leaked
  as file-scope funcs (DOOM's `r_vulkan.cpp` — `RB_VulkanProbe` +
  `RB_Vulkan_*` entry points, with `devs`/`exts` leaking).

## Pre-fix check

Against pre-fix code INV-1/2/3/4 FAIL (locals/case-labels tagged func;
old-style/brace-next-line defs missed) and INV-8 FAILS (`extern "C"`
defs undetected; their locals leak). INV-5/6 pass before and after
(regression guard). Verified before the fix.

Label: `features;fast`.
