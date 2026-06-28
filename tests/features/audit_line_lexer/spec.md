# Feature: per-language comment/string lexer is a testable surface (ANTS-2210)

## Problem

ANTS-1270 added extension-dispatched comment/string lexing to
`AuditDialog::lineIsCode` so the audit pipeline stops treating `#`
(Python/shell) and Lua `--` / `[[ ]]` comments as code (which let
IP/secret/TODO findings inside comments survive). But the lexer was a
private static on the GUI dialog (`ants_audit_dialog_lib`), and the audit
feature tests link only `ants_audit_lib` + `ants_core_lib` — so the lexer
had no direct regression test and could silently regress.

ANTS-2210 moves the pure lexer to `AuditHygiene::lineHasCode(source, path,
line)` in `ants_audit_lib` (the GUI `lineIsCode` reads + 2 MB-caps the file
and forwards `source` here). This test locks the behaviour to that pure
surface.

## Contract — `AuditHygiene::lineHasCode(source, path, line)`

`source` is the file text, `path` selects the syntax by extension, `line`
is 1-based. Returns true iff the line carries a real identifier/operator
character outside any comment or string; false when the line is wholly
comment/string. Safe default is true (preserve findings).

- **AL-1 Python `#` comment** — a `# password = "10.0.0.1"` line in a `.py`
  file is non-code (false); an adjacent `x = 1` assignment is code (true).
- **AL-2 Shell `#` comment** — a `# secret` line in a `.sh` file is non-code.
- **AL-3 Lua comments** — `.lua`: a `-- note` line is non-code; a `--[[ … ]]`
  block comment's interior line is non-code; a `[[ long string ]]` interior
  line is non-code; a real `local x = 1` line is code.
- **AL-4 C-style comments** — `.cpp`: a `// note`-only line and the interior
  of a `/* … */` block are non-code; an `int x;` line is code. ANTS-2230 — the
  `//` and `/*` introducers are excluded from code-detection (the C-style
  analogue of ANTS-1270's `#`/`--` exclusion), so a finding inside a
  comment-only C/C++ line is dropped.
- **AL-4b division stays code** — a bare `/` division operator (`a / b`) still
  reads as code; only the comment-opening `//` and `/*` are excluded.
- **AL-5 C++ raw string** — `.cpp`: a `const char *p = R"(a//b"c)";` line is
  code (it has the `const`/identifier), and the raw-string body does NOT
  desync the lexer — the immediately following `int y;` line is still code.
- **AL-6 Python triple-quote** — `.py`: lines wholly inside a `""" … """`
  docstring are non-code.
- **AL-7 default C-style** — an unknown/empty extension uses the C-style
  lexer, where `#` is not a comment introducer: the same `# note` line is
  non-code under `.py` (Hash) but code under the default lexer.
- **AL-8 guard** — `line <= 0` returns true (safe default).
