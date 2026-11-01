# `source_scrapes` — ANTS-5602

The tests that read a given source file as text, and the strings each one
looks for in it. A refactor that renames or moves one of those strings
breaks the test without touching any code the test calls. The shape was
agreed with claude-config (CFG-0830, 2026-10-19). The seam is
`src/sourcescrapes.{h,cpp}`; the CMake half is
`BuildTargets::pathDefinitions` in `src/buildtargets.{h,cpp}`.

## How a test reaches a file

- **A path definition.** A `NAME="${CMAKE_SOURCE_DIR}/rel"` definition in
  the root `CMakeLists.txt` makes the token `NAME` read `rel`. A definition
  naming a variable reads every path of the list that variable was built
  from, through `set`, `list(APPEND)`, `list(TRANSFORM … OUTPUT_VARIABLE)`
  and `list(JOIN)`.
- **A directory definition.** A definition naming a directory makes a
  source-shaped string literal, in a test file that names the token, read
  the file it names under that directory, where that file exists.
- **A helper.** A function whose `return` reads a path reads it at every
  call. Functions in headers under the test roots are helpers in every test
  file; the others only in their own file.
- **A variable.** A variable assigned from a statement that reads a path
  reads it too, until the end of the enclosing test or function.
- **A Python test** (ANTS-6180). `getsource(x)` or `Path(x.__file__).read_text`
  reads the module `x` was imported from, under `source_roots` or the root. A
  join from `Path(__file__)`, or from a variable holding a repository path,
  reads the existing file it names. So a test that runs a script shows too.
- **A shell test** (ANTS-6180). `$VAR/<path>` reads the existing file it names,
  where `VAR` holds the script's own directory, the git top level, the script's
  `$1` at the top level, or a known variable joined to a path. Any other
  variable, such as a `mktemp` directory, reads nothing.

## Invariants

| # | Invariant | Test |
|---|---|---|
| 1 | A path definition, or a definition built from a list, makes its token read each path. A row is `{path, test, file, line, anchor, kind}`: `test` is `Suite.Name` from the enclosing `TEST`, `TEST_F` or `TEST_P`, `file` the test file, `line` the anchor's line, `anchor` the literal's text as written between its quotes. | `SourceScrapes.Inv1DefinitionsReadTheirPaths` |
| 2 | A directory definition makes a source-shaped literal read the existing file it names; a literal naming no existing file reads nothing. | `SourceScrapes.Inv2DirectoryDefinitionAndLiteral` |
| 3 | A header helper under the test roots reads its paths in every test file that calls it. | `SourceScrapes.Inv3HeaderHelperReads` |
| 4 | A binding ends with its test: a variable of the same name in the next test reads only what it was assigned there. A variable assigned from a bound variable is bound to the same paths. | `SourceScrapes.Inv4BindingsAreScopedAndFollowed` |
| 5 | A literal argument of `find`, `rfind`, `contains`, `indexOf`, `lastIndexOf` or `count` on a reading receiver, or of a call whose first argument is a bound variable, is an anchor of kind `literal`. A literal given to `std::regex` or `QRegularExpression` and used on a bound variable is of kind `regex`. | `SourceScrapes.Inv5LiteralAndRegexAnchors` |
| 6 | Comments and string contents never read a path. A variable holding only a literal is that literal, reported at the literal's line. Any other variable anchor is counted in `scanned.unresolved_anchors`, not reported. | `SourceScrapes.Inv6CommentsStringsAndVariables` |
| 7 | Only rows reading a requested path are returned, ordered by path, file and line. `readers` lists each test file and test that reads a requested path directly, so a test with no captured anchor is still named. `limit` caps each list (default 500); `scanned` holds the totals, and `scanned.rows_truncated` is true when the cap cut either list (`scanned.truncated` is the file walk's own cap). | `SourceScrapes.Inv7FilteredAndOrdered` |
| 8 | Neither `path` nor `paths`, a non-string entry, or `limit` outside 1 to 5000 refuses `bad_args`. A path outside the root or missing refuses `bad_path`. | `SourceScrapes.Inv8Refusals` |
| 9 | The verb is registered in `registerProjectScopedVerbs` with `CallerCwdContract::Required`, matches `callerCwdContractFor`, and its schema is a closed object. | `SourceScrapes.Inv9Wiring` |
| 10 | In a Python test, `getsource` or a `__file__` read of an imported name gives a reader row: `test` is the enclosing `test*` function, `via` the call. A name that is no module, or an attribute of one, reads the nearest module above it. | `SourceScrapes.Inv10PythonGetsourceReadsItsModule` |
| 11 | A Python join from `Path(__file__)` or a variable holding a repository path gives a reader row. A join from any other name, a comment and a triple-quoted string give none. | `SourceScrapes.Inv11PythonJoinsFromTheTestFile` |
| 12 | A shell `$VAR/<path>` gives a reader row only where `VAR` holds a repository path as "A shell test" says. A comment, a `mktemp` variable, a function's `$1` and a variable rebound to anything else give none. | `SourceScrapes.Inv12ShellJoinsFromAKnownRoot` |
| 13 | `scanned.test_files` counts C++, Python and shell files. At zero, `notices` says empty rows do not mean nothing reads the path, and names the test roots. Otherwise `notices` is absent. | `SourceScrapes.Inv13NoticeWhenNoTestFileWasRead` |

## Not covered

A path built at run time (`ANTS_SOURCE_DIR "/" + rel`), a definition in a
`CMakeLists.txt` other than the root one, and a file reached through an
environment variable. In Python, a module passed through a variable, a path
built with `os.path`, and a file found by walking a directory. Anchors in a
Python or shell test: those tests appear in `readers` only. Test files in any
other language. `not_covered` in every reply says so.
