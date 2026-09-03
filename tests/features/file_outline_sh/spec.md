# Feature spec: `file_outline` shell lane (ANTS-4826)

Status: shipped
Kind: enhancement
Source: cc-feedback-2026-09-03 (DOOM, OneUp, RetroDB — three projects,
independently)

## Problem

`file_outline`'s mode enum ran auto|cpp|py|md|json|generic|glsl|html. A shell
script matched none of them, so the verb answered `{language:"unknown"}` with
no `symbols` array at all.

`SymbolQuery::langForExt` has mapped `.sh` and `.bash` to `Lang::Sh` all
along, so `find_definition` and `workspace_search` both advertise `sh`. Two
verbs, two answers about whether shell is a supported language — the same
drift ANTS-4096 fixed for shaders and ANTS-4425 for HTML, a third time.

Three projects reported it in one week. Shell is where a project's release and
launcher logic lives, which makes those the files a session most wants to
orient in without paying for a full Read; all three fell back to `grep` or
`cat`. One of them was told to report it by the verb's own hint.

## Surface

- `FileOutline::Mode::Sh` — new mode, reachable by extension, by an explicit
  `mode:"sh"`, and by shebang.
- `FileOutline::isShExt(ext)` — exported, so `CodebaseIndex::isIndexableSuffix`
  delegates rather than growing a second list.
- `FileOutline::isShShebang(firstLine)` — exported and unit-reachable.
- `CodebaseIndex::isIndexableSuffix` admits the same pair.
- The `file_outline` schema lists `sh` in its `mode` enum.

Landmarks, not a parse: both function spellings and top-level assignments.
All three rules anchor at column 0 with no leading-whitespace class, for the
reason `rxGenericBinding` gives — indentation is the only cheap signal a
line-based scanner has for "top level", and a script's body locals outnumber
its landmarks.

## Invariants

- **INV-1 extensions.** A `.sh` or `.bash` file reports `language:"sh"` and
  yields both function spellings — `name() {` and `function name {` — with
  kind `func`.
- **INV-2 top-level bindings only.** A top-level assignment is emitted with
  kind `const`, including behind `export` / `readonly` / `declare`. An
  indented `local` inside a function body is not. The second half is the
  control: without it a rule that emitted every `name=` line would pass, and
  those lines are the majority.
- **INV-3 explicit mode.** `mode:"sh"` is honoured, so a caller can force the
  lane for a project-specific extension. Breaks when `parseMode` silently
  returns `Auto`, which reads as "unknown language" rather than as a rejected
  argument.
- **INV-4 shebang.** An extensionless script is recognised from a shell
  shebang, in both the direct and `env` forms. Named by the reporters, because
  extensionless is the normal shape for a hook, a launcher or a CI gate — and
  the extension is the only signal `pickModeByExt` has.
- **INV-5 no shebang stays unknown.** A file with neither a known extension
  nor a shell shebang is still `unknown`. The control for INV-4: a peek that
  fired on any `#!` would claim every extensionless Python and Perl file in
  the tree. Passes in both states.
- **INV-6 the index agrees.** `CodebaseIndex::isIndexableSuffix` admits the
  same extensions. `codebaseindex.cpp`'s own in-step rule is that count →
  outline → symbol query cover the same files, and it is the rule ANTS-4096
  and ANTS-4425 were each filed to restore after exactly this drift. Adding
  the outline lane without this gate would repeat it.

## Deliberately not done

`.zsh` and `.ksh` are not claimed, because `SymbolQuery::langForExt` does not
claim them either. Widening the set is a change to that lane first; this verb
must not be the one that makes the three disagree again in the other
direction.
