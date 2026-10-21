# doc_lint — checks chosen by declared genre (ANTS-5293)

`genre:"auto"` makes `doc_lint` read each document's own `Genre:` line and
pick that document's checks from it. Without it, `spec_lint` runs only on
documents under `specs_dir` (ANTS-3663 § 2.1). The lean workflow says a
genre is declared in the file and never inferred from its path.

## Contract

- **The argument.** `genre` takes `"path"` (the default, today's behaviour)
  or `"auto"`. Any other value refuses `bad_args`.
- **The declaration.** A line `Genre: <word>` in the document's first 25
  lines, matched by `~/.claude/tools/check-doc.py`'s `GENRE_RE`: optional
  list marker, optional bold, optional colon, the word optionally in
  backticks, nothing after it. The word is lower-cased.
- **The table.** A declared genre selects these checks; the rest do not run
  on that document. It follows `check-doc.py`'s `GENRE_CHECKS`.

  | Genre | Checks |
  |---|---|
  | `spec` | all six |
  | `plan`, `adr`, `standard` | all but `spec_lint` |
  | `prose` | `doc_integrity`, `doc_dedup`, `doc_facts` |
  | `instructions` | `doc_integrity`, `doc_citations` |
  | `record` | `doc_integrity` |

  Why each row: `spec_lint` checks the spec format's sections, which only a
  spec carries. Prose describes other projects, so its paths and symbols are
  not this tree's; `check-doc.py` drops `path` and `symbol` for it. An
  instructions document's cited paths are its steps. A record is frozen: its
  quotations, counts and versions describe the date it was written.
- **The fallback.** A document with no declaration, or naming a genre not in
  the table, is judged as before: every check, and `spec_lint` by path.
- **The report.** Under `genre:"auto"` the response carries `genres`
  (file → the genre it was judged as) and `genre_unknown` (file → the word
  it declared, judged by path). Each is present only when non-empty.
  Under `genre:"path"` neither appears.
- **`checks_run`** keeps its meaning: a checker is listed when it ran on at
  least one document.

**Reload.** Ships in `ants-mcpd`; rebuild it and reconnect (`/mcp`). No
terminal relaunch. It holds no running state, so it has no stop/start
control of its own.

## Rows

| Test | Asserts |
|---|---|
| `DocLintGenre.DeclarationShapes` | each accepted line shape parses; a line past 25, trailing text, or no line reads as none |
| `DocLintGenre.TableMatchesSpec` | `genreChecks` returns this file's table, and nullptr for an unknown genre |
| `DocLintGenre.DeclaredSpecOutsideSpecsDirIsLinted` | `spec_lint` runs on a `Genre: spec` document outside `specs_dir` |
| `DocLintGenre.DeclaredRecordInsideSpecsDirIsNot` | `spec_lint` does not run on a `Genre: record` document under `specs_dir` |
| `DocLintGenre.ProseDropsCitationsAndSymbols` | a lone prose document runs exactly its three checks |
| `DocLintGenre.UndeclaredAndUnknownKeepThePathRule` | both fall back; `genres` and `unknownGenres` say which was which |
| `DocLintGenre.PathModeIgnoresTheDeclaration` | without `genreAuto` a declared spec outside `specs_dir` is not linted, and nothing is reported |
| `DocLintVerb.GenreArgReachesTheEngine` | through `cmdDocLint`: `genre:"auto"` reports `genres` and `genre_unknown`, the default reports neither, `"poem"` refuses `bad_args` |
