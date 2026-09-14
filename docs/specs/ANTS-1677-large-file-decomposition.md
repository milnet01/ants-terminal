# ANTS-1677 — Split `mainwindow.cpp`, `auditdialog.cpp` and `claudeintegration.cpp` under one decomposition contract

**Status:** accepted (2026-09-14).
**Kind:** refactor.
**Source:** ROADMAP.md ANTS-1677 (indie-review-2026-04-27 trio note; shared-contract ruling by the user 2026-09-07; written next per the user 2026-09-14).
**Covers:** ANTS-1043, ANTS-1044, ANTS-4919.
**Pairs with:** ANTS-3833 (`remotecontrol.cpp` split, shipped — the precedent whose linkage rules this spec reuses).

No user-visible change. The three largest source files become several smaller
ones, so a change to one feature stops locking the whole file, and parallel
Claude sessions can work on neighbouring features without colliding.

## 1. Problem

Three classes each live in one oversized translation unit (TU). `wc -l` measures
each file, and `file_outline sizes:true` measures its functions.

| File | Where the bulk sits |
|---|---|
| `src/claudeintegration.cpp` | `ClaudeIntegration::onMcpConnection()` |
| `src/mainwindow.cpp` | `MainWindow::setupClaudeMcpProviders()` and the constructor |
| `src/auditdialog.cpp` | `AuditDialog::populateChecks()` |

Four consequences:

1. **Colony throughput.** ADR-0005 § Consequences makes file size a throughput
   variable: a task touching one of these files excludes every other task that
   would. The user ruled on 2026-09-07 that these splits run before Colony.
2. **Edit cost.** One edit recompiles the whole TU, and no session can hold the
   file, so every change is a blind targeted edit — the problem ANTS-3833
   § 1 measured for `remotecontrol.cpp`.
3. **The bulk sits inside a few giant functions, not across many members.**
   ANTS-3833 moved whole members between files. That alone cannot split
   `onMcpConnection()`, whose `method == "tools/list"` branch builds every tool
   descriptor in sequence. This command prints the branch's length:
   `awk '/method == "tools\/list"/{a=NR} /method == "tools\/call"/{print NR-a; exit}' src/claudeintegration.cpp`.
   The same holds for the `registerToolProvider(` calls inside
   `setupClaudeMcpProviders()`. This command counts them:
   `awk 'NR>=s && NR<=e && /registerToolProvider\(/' src/mainwindow.cpp | wc -l`,
   with `s` and `e` set to that function's first and last lines.
4. **Many things read these files as text, and some fail silently when the
   text moves.** The command in § 2.6 prints the per-file figures.
   Two scripts also read a file: `tests/audit_self_test.sh` reads
   `auditdialog.cpp`, and `tools/check-readme-claims.sh` reads
   `claudeintegration.cpp`.

   **Four readers look at one file of a class. Two break silently when the text
   moves:**
   - `tests/audit_self_test.sh`'s fixture-coverage block extracts every
     `addGrepCheck("<id>"` from `src/auditdialog.cpp`. With no match its loop
     runs zero times and it reports nothing missing.
   - The product's own `audit_fixture_coverage` check, built in
     `AuditDialog::populateChecks()`, runs the same grep over
     `src/auditdialog.cpp`. Its comment says it is a no-op when the grep yields
     no ids.

   **Two break loudly, for the wrong reason:**
   - `tools/check-readme-claims.sh` counts MCP tools with a `["name"] = "…";`
     grep over `src/claudeintegration.cpp` alone, and exits 1 on any mismatch
     with `README.md`. After a split it reports README drift that is not there.
   - `tests/features/mcp_tools_list_schema` counts `tools.append(` and
     `inputSchema` between the `"tools/list"` and `"tools/call"` markers, and
     expects both counts above zero. After the descriptors move out, it fails.

## 2. Surface

### 2.1 Decisions already made

| Decision | Made by |
|---|---|
| One shared contract for ANTS-1043, 1044, 1049 and 4919, not four specs | user, 2026-09-07 |
| The splits run before Colony, as Colony enablers | user, 2026-09-06 and 2026-09-07 |
| Order of the work: ANTS-1044, ANTS-1043, ANTS-4919 | user, 2026-09-14 |
| ANTS-1049's data table leaves this contract for its own later spec; the files are split first | user, 2026-09-14, after implementation found `AuditDialog::populateChecks()` gates checks on installed tools, a config file and the filesystem, and assembles some commands at runtime |
| `tools/list` keeps today's order; regrouping by family is a later, separate change | user, 2026-09-14 |
| The umbrella is filed under ANTS-1677 with `**Covers:**` | author — `docs/standards/specs.md` § 2 permits the umbrella form in this project, and ANTS-1677 is the item naming all three files |
| Piece naming `src/<stem>_<part>.cpp`, where `<stem>` is `mainwindow`, `auditdialog` or `claudeintegration` | author — § 2.3 says why |
| A 4,000-line cap per file in a class's source list | author — § 2.5 |

### 2.2 Two kinds of move

Every change this spec authorises is one of two kinds. A commit may contain
only one kind for a given class, so each commit carries exactly one proof.

| Kind | What moves | Proof that nothing changed |
|---|---|---|
| **A — member motion** | whole member function bodies, or a namespace-scope definition, cut as contiguous slices from one file of the class's list into another, new or existing | INV-7 (motion identity) and INV-10 |
| **B — function carving** | a run of consecutive statements inside one function becomes the body of a new function; the original body calls the new functions in the original order | INV-7 (motion identity) and INV-10 |

**Every cut of `claudeintegration`, of any kind, also carries INV-6.**

**Kind B's new functions.** For `ClaudeIntegration`, each carved piece of the
`tools/list` branch is a free function
`void appendToolSchemas<Part>(QJsonArray &tools)` declared in
`src/claudeintegration_internal.h`. For `MainWindow`, each carved run of
`registerToolProvider` calls becomes a private member
`void MainWindow::registerMcpProviders<Part>()`, taking no parameters. Private
because the lambdas capture `this` and read private members. Locals that several
pieces share are handled before the cut as § 2.4 says. Today the registrations
share `kRcUnavailable` and `rcDelegate`, and the `tools/list` branch shares
`makeCallerCwdReadProp` and its siblings. `mcpOn` is read only above the first
`registerToolProvider` call, so it stays in `setupClaudeMcpProviders()`.

**No code-to-data move is authorised here.** ANTS-1049's table waits for its own
spec (§ 2.1). ANTS-1044 moves `AuditDialog::populateChecks()` unchanged, by
kind A, into `src/auditdialog_catalogue.cpp` — the name § 2.3's rule gives in
place of the `auditcatalogue.cpp` ANTS-1044's body proposes.

### 2.3 Source lists and the text readers

ANTS-3833 § 2.4 made the class's file list the single answer to *which files
are this class's source*. This spec applies that answer to three more classes
and adds a second route to it, for readers that cannot see a CMake definition.

**Per class, in `CMakeLists.txt`:**

```cmake
# ANTS-1677 — entry 1 is src/<stem>.cpp; the pieces follow in ascending order of
# where their text began in it.
set(ANTS_MAINWINDOW_SOURCES_REL        src/mainwindow.cpp        …)
set(ANTS_AUDITDIALOG_SOURCES_REL       src/auditdialog.cpp       …)
set(ANTS_CLAUDEINTEGRATION_SOURCES_REL src/claudeintegration.cpp …)
```

- The owning library consumes the list rather than restating the files:
  `ants_chrome_lib` for `mainwindow`, `ants_audit_dialog_lib` for
  `auditdialog`, `ants_claude_lib` for `claudeintegration`
  (`build_target_for` on each file, 2026-09-14).
- Every test bundle that reads the class's text gets
  `ANTS_<STEM>_SOURCES="<abs1>;<abs2>;…"` — the same `;` separator,
  escaped the same way, as ANTS-3833's `ANTS_RC_SOURCES`. Unlike that list, the
  concatenation does not keep every line's pre-split position, because text after
  a cut stays in entry 1.

**For shell and Python readers, the glob.** A script cannot read a compile
definition, so it reads `src/<stem>.cpp src/<stem>_*.cpp`. INV-2 makes that glob
and the CMake list the same set, which is why the naming rule exists. The glob is
safe today: `ls src/mainwindow_*.cpp src/auditdialog_*.cpp src/claudeintegration_*.cpp`
finds no file. The underscore is load-bearing. A glob without it
(`src/remotecontrol*.cpp`) also matched `src/remotecontrolgate.cpp`, and
ANTS-3833 § 2.3 had to forbid globs for exactly that reason.

**Test helper, in `tests/_support/srcgrep.h`:**

```cpp
// ANTS-1677 — read every path in a `;`-separated source list, in order,
// joined with '\n'. Unguarded: it takes the list as an argument, so it
// compiles in every bundle.
inline std::string slurpSourceList(const char *list);

#if defined(ANTS_MAINWINDOW_SOURCES)
inline std::string slurpMainWindow()        { return slurpSourceList(ANTS_MAINWINDOW_SOURCES); }
#endif
#if defined(ANTS_AUDITDIALOG_SOURCES)
inline std::string slurpAuditDialog()       { return slurpSourceList(ANTS_AUDITDIALOG_SOURCES); }
#endif
#if defined(ANTS_CLAUDEINTEGRATION_SOURCES)
inline std::string slurpClaudeIntegration() { return slurpSourceList(ANTS_CLAUDEINTEGRATION_SOURCES); }
#endif
```

**The single-file path macros are deleted, class by class, in the groundwork
commit of the first item that adds a file to that class, while its list still
holds one entry.** That turns every missed call site into a compile error, the
same trade ANTS-3833 § 2.4(b) made. Inventory, from
`grep -oE '[A-Z0-9_]+="\$\{CMAKE_SOURCE_DIR\}/src/<stem>\.cpp"' CMakeLists.txt | cut -d= -f1 | sort -u`:

| Class | Macros deleted |
|---|---|
| `mainwindow` | `SRC_MAINWINDOW_CPP`, `SRC_MAINWINDOW_CPP_PATH`, `SRC_MAINWINDOW_PATH`, `MAINWINDOW_CPP` |
| `auditdialog` | `SRC_AUDIT_CPP`, `SRC_AUDIT_CPP_PATH`, `SRC_AUDITDIALOG_CPP_PATH`, `SRC_AUDITDIALOG_PATH` |
| `claudeintegration` | `SRC_CLAUDE_INTEGRATION_CPP_PATH` |

The header macros (`SRC_MAINWINDOW_H`, `SRC_MAINWINDOW_H_PATH`,
`SRC_AUDITDIALOG_H_PATH`) and `SRC_TESTAUDITDIALOG_CPP_PATH` are for other files
and stay. **Before a macro is deleted, its non-text uses are re-routed.** That
covers `QStringLiteral(…)` used as a directory handle, `QFileInfo(…)`, and
`#if defined(…)` guards, as ANTS-3833 § 2.4 did. The command in § 2.6 lists the
use forms per class.

**The four readers of § 1 are re-pointed in the same commit that moves their
subject, with one exception.** The `audit_fixture_coverage` command is text
inside `AuditDialog::populateChecks()`, so it is re-pointed in ANTS-1044's
groundwork commit, while the class is still one file, before the cut moves it.
**The two silent readers also gain an emptiness check.**

| Reader | Reads after the move | Failure when its subject is missing |
|---|---|---|
| `tests/audit_self_test.sh` fixture coverage | the ids from the whole `auditdialog` glob | new: `FAIL` when the extracted id list is empty |
| `audit_fixture_coverage` runtime check | the same set, from the same files | new: a finding when its grep yields no ids in a tree where `src/auditdialog.cpp` exists |
| `tools/check-readme-claims.sh` MCP tool count | the `claudeintegration` glob | existing: exit 1 on a count that differs from `README.md` |
| `tests/features/mcp_tools_list_schema` | the carved `appendToolSchemas*` bodies, via `slurpClaudeIntegration()` | existing: its `appendCount > 0` and `schemaCount > 0` expectations |

**INV-5 pins the id set both readers return**, whichever file of the class
holds the calls.

### 2.4 Linkage

**ANTS-3833 § 2.3 applies unchanged, with `<stem>detail` in place of
`rcdetail` and `src/<stem>_internal.h` in place of
`src/remotecontrol_internal.h`.** That covers: which symbols are promoted, the
derivation from the source list and never from a glob, definitions staying in a
`.cpp`, the `extern const` rule, the forced exceptions for types and
`inline constexpr`, and one `using namespace <stem>detail;` per piece. It is not
restated here.

Additions:

- **The promotion set is derived from the item's seam plan (§ 2.5), fixed before
  its groundwork commit.** ANTS-3833 derived it from the cut files; here the
  groundwork commit comes before any cut, so the seam plan's slices stand in
  for them.
- **Kind B promotes locals, not only file-scope symbols.** A local constant, or
  a lambda that does not capture `this`, used by more than one carved piece
  becomes a `<stem>detail` constant or function before the cut. A local lambda
  that captures `this` becomes a private member function with the same name and
  call shape, so its call sites' text is unchanged — `MainWindow`'s
  `rcDelegate` is the case today.
- **The file-scope scan uses the shipped scanner.**
  `tools/rc-namespace-scan.py` takes any source path, and
  with `--ns anon,<stem>detail` lists both kinds of block. Both are
  needed: after § 2.4's promotion an anonymous-only scan finds nothing and reports
  every seam as open code. Every seam of a kind A cut is
  checked against it, `--seams` included, before code moves.

### 2.5 Where the seams go

**The seam plan is made per item, before its groundwork commit, and recorded in that
commit's message. It is not fixed here.** Line numbers in these files move with
every edit, and ANTS-3833 § 2.2 already had to declare its own seam lines
evidence rather than coordinates. What this spec fixes is the rule a seam plan
must satisfy:

1. **A piece is a unit one task edits alone.** Cut along the concerns already
   visible in the code: a menu, a tool family's run of descriptors, a dialog
   section, an export format. Never cut through a function except by kind B.
2. **Every file in a class's list stays at or under 4,000 lines**, including
   `src/<stem>.cpp` itself (INV-9). The cap is set below ANTS-3833 INV-6's cap
   because the purpose here is parallel lanes rather than compile time alone. A
   piece that would pass it is split again, at the next concern boundary.
3. **`tools/list` pieces are contiguous runs of today's descriptor order**, and
   `onMcpConnection()` calls them in that order — the user's 2026-09-14 ruling.
   A piece is named for the dominant family in its run.
4. **A new feature's code goes into the piece owning its concern**, never back
   into `src/<stem>.cpp` when a piece fits. This is what keeps INV-9 from being
   the only defence against the file regrowing.

### 2.6 Measuring the text readers

Re-run it at cut time; its output is evidence, not a constant.

```bash
for f in mainwindow auditdialog claudeintegration; do
  M=$(grep -oE "[A-Z0-9_]+=\"\\\$\{CMAKE_SOURCE_DIR\}/src/$f\.cpp\"" CMakeLists.txt | cut -d= -f1 | sort -u | paste -sd'|')
  pat="src/$f\.cpp|\b($M)\b"
  readers=$(grep -rlE "$pat" tests --include='*.cpp')
  echo "$f macros=$(echo "$M" | tr '|' '\n' | grep -c .)" \
       "files=$(echo "$readers" | grep -c .)" \
       "sites=$(grep -rohE "$pat" tests --include='*.cpp' | wc -l)" \
       "window_files=$(grep -lE '\.(substr|mid|left)\(' $readers | wc -l)" \
       "scripts=$(grep -rlE "src/$f\.cpp" tools tests --include='*.sh' --include='*.py' | paste -sd' ')"
  grep -rhoE "[A-Za-z_]+\((${M})\)" tests --include='*.cpp' | sort | uniq -c   # use forms
done
```

### 2.7 Commit shape, per item

Each item lands as ordered commits, and every one builds and passes the suite:

| # | Contents |
|---|---|
| 1 | **Checks first, while the file is still one TU.** For the first item needing each: `tools/split-motion-check.py` (INV-7 and INV-10), the INV-6 capture harness, `tests/features/split_sources`, the audit readers case with `tests/audit_self_test.sh --list-rule-ids`, and the seams INV-1 allows. Nothing else changes, so every later commit is measured against a parent that already has its checks. |
| 2 | **Groundwork.** The class's `ANTS_<STEM>_SOURCES_REL` list, holding one entry; the macro definitions; the `srcgrep.h` wrapper; every text reader migrated to `slurp<Stem>()` and the single-file macros deleted; the re-route of non-text macro uses; and the promotion of § 2.4. |
| 3 | **The cuts.** One or more commits, each carrying one kind of move and compared with its parent by the invariants § 2.2 names for that kind. The list grows, and each reader of § 1 is re-pointed in the commit that moves its subject, apart from the exception § 2.3 names. A cut needing a symbol the groundwork did not promote is preceded by its own promotion commit. |
| 4 | Any standing test of § 6 for that class that row 1 did not add. |

**Items and classes.** ANTS-1044 splits `auditdialog`, ANTS-1043 `mainwindow`,
and ANTS-4919 `claudeintegration`, in that order. `slurpSourceList`,
`split_sources` and `tools/split-motion-check.py` land with ANTS-1044, the first
item.

## 3. Invariants

**When each invariant applies.** INV-9 holds for a class from the last cut
commit of the last item that splits it. INV-3 holds from the groundwork commit of
the first item that adds a file to the class; INV-2 and INV-11 from the first
commit that adds a file. INV-6, INV-7 and INV-10 compare each cut commit
with its parent, for the kinds § 2.2 assigns them; INV-6 (for
`claudeintegration`) and INV-10 also compare each commit that promotes a symbol
(§ 2.4) with its parent. INV-1, INV-4, INV-5 and
INV-12 hold at every commit.

- **INV-1** — No public or protected declaration of `MainWindow`, `AuditDialog`
  or `ClaudeIntegration` changes. The only header edits allowed are added
  private member declarations — for kind B pieces and for § 2.4's promotions of
  `this`-capturing lambdas — plus one public
  `QList<AuditCheck> AuditDialog::checksForTest() const`, INV-5's seam, named
  with the project's `ForTest` suffix. *Breaks when:* a carved piece is
  exposed publicly, or a signature changes in passing, which recompiles and can
  re-bind every consumer for no reason. *Test:* for each item, review
  `git diff <item-parent>..<item-last-commit> -- src/mainwindow.h src/auditdialog.h src/claudeintegration.h`;
  every added line must sit under `private:`, apart from that one seam, and
  every removed line must be none.
- **INV-2** — For each split class, `ANTS_<STEM>_SOURCES_REL` lists
  `src/<stem>.cpp` first. As a set, it equals the files matching
  `src/<stem>.cpp src/<stem>_*.cpp`, and the owning library consumes it.
  *Breaks when:* a piece is added to `add_library()` and not to the list (scrapes
  read a fraction of the class), or a file matching the glob is not in the list
  (scripts read something the build does not). *Test:*
  `tests/features/split_sources` compares the list parsed from `CMakeLists.txt`
  with a directory listing of `src/`, for each class whose list has more than
  one entry. It also splits each compiled `ANTS_<STEM>_SOURCES` on `;` and
  asserts the entries equal the parsed list, which catches a separator left
  unescaped — `CMakeLists.txt` records that failure beside `ANTS_RC_SOURCES`.
- **INV-3** — From the point § 3's opening names, none of the class's
  single-file path macros (§ 2.3) survives in `CMakeLists.txt` or in a `.cpp`
  file under `tests/`, and no `.cpp` file under `tests/` reads the class's text
  through a literal `src/<stem>.cpp` path. A mention that reads nothing — a
  comment, a message, a path handed to a verb as input — is outside it, as are
  shell and Python readers using § 2.3's glob and prose in `spec.md` files.
  *Breaks when:* a bundle keeps a deleted macro or a test opens the literal path,
  so a scrape reads one piece and reads a moved function as deleted. *Test:*
  with `$M` the alternation of the class's macro names in § 2.3's table,
  `grep -rlE --include='*.cpp' "\b($M)\b" tests/ | wc -l` prints `0` and
  `grep -cE "\b($M)\b" CMakeLists.txt` prints `0`. The literal-path clause is a
  recorded review: `grep -rnE --include='*.cpp' 'src/<stem>\.cpp' tests/` lists
  the remaining mentions, and the groundwork commit's message says why each
  reads nothing.
- **INV-4** — `tools/check-readme-claims.sh` counts MCP tools over the whole
  `claudeintegration` glob. *Breaks when:* the count still reads
  `src/claudeintegration.cpp` alone, so after a cut it reports README drift that
  is not there and a correct push is refused. *Test:*
  `bash tools/check-readme-claims.sh` exits 0 after each `claudeintegration` cut
  commit.
- **INV-5** — The rule-id set extracted by `tests/audit_self_test.sh`'s
  fixture-coverage block equals the id set extracted by the
  `audit_fixture_coverage` runtime check. Both are non-empty.
  *Breaks when:* ANTS-1044 moves the calls into a file one reader does not
  read — the reader then extracts nothing and passes. *Test:* a case in the
  audit bundle takes `audit_fixture_coverage`'s command from
  `AuditDialog::checksForTest()`, then runs it and
  `tests/audit_self_test.sh --list-rule-ids` — a mode ANTS-1044's first commit
  adds, printing the extracted ids one per line and exiting — over a copy of the tree with no `tests/audit_fixtures/`, so the
  runtime check reports every id. It asserts the two extracted sets are equal
  and non-empty.
- **INV-6** — The live `tools/list` response is unchanged by a `claudeintegration`
  cut, byte for byte. *Breaks when:* a carved piece is called out of order, or a
  descriptor is edited, dropped or duplicated while moving. *Test:*
  `tests/features/mcp_tools_list_live` starts a `ClaudeIntegration` with
  `startMcpServer()` on a temporary socket. It sends `initialize` then
  `tools/list` with no `_meta.shape`, which returns the full shape (ANTS-1502
  makes the lite shape opt-in). It writes the response to
  `$ANTS_TOOLS_LIST_DUMP` when that variable is set. Run before and after
  each `claudeintegration` cut or promotion commit, the two dumps compare equal
  with `cmp`. Its standing assertion is
  that the response's tool-name set equals the set of names on the lines of
  `slurpClaudeIntegration()` that match `tools/check-readme-claims.sh`'s MCP-tool
  pattern — a line inside an `#ifdef ANTS_LUA_PLUGINS` block counting only when
  the test build defines `ANTS_LUA_PLUGINS` — and is not empty.
- **INV-7** — A kind A or kind B cut is motion. Every line removed from
  a file of the class's list appears byte-identical exactly once elsewhere in that
  list — in another file, or, for a kind B carve kept in the same file, in that
  file's new function. Lines keep their relative order within each piece. Every other
  added line is one of: a blank line; a piece's ordinal marker; an `#include`;
  a preprocessor conditional line (`#if…`, `#else`, `#endif`) repeating one that
  enclosed the moved text; `namespace <stem>detail {` or its closing brace; a
  `using namespace <stem>detail;`; a carved function's signature or closing
  brace; or a call to a carved function. The ordinal marker is a piece's first
  line, `// ANTS-1677 <stem> piece <k>/<N> — <concern in plain words>`, and like
  ANTS-3833 § 2.4(c)'s marker it carries no snake_case identifier.
  Entry 1, `src/<stem>.cpp`, carries none. `<k>` is the piece's position in the
  list and `<N>` the list's length after the commit that writes the marker, so a
  cut that adds a piece rewrites every marker, and INV-7 accepts a marker line
  replaced by its renumbered form. *Breaks
  when:* a statement is edited, reordered or dropped during the cut. *Test:*
  `tools/split-motion-check.py --pre <parent> --post <cut> --stem <stem>` exits
  0 for each kind A or kind B cut commit. Against a scratch cut with one moved
  line altered, it exits non-zero.
- **INV-8** — *withdrawn — 2026-09-14: ANTS-1049's code-to-data move left this
  contract (§ 2.1), so no item makes the catalogue-identity comparison.*
- **INV-9** — No file in a split class's source list exceeds 4,000 lines.
  *Breaks when:* code re-accretes into one file until the split is undone, the
  one regression that returns silently. *Test:* `tests/features/split_sources`
  counts lines per listed file, for
  the classes named in its own capped-class list; the last cut commit of a
  class's last item adds that class to the list.
- **INV-10** — Every scrape whose region's content a cut, or a promotion commit (§ 2.4),
  changes has its window-cutting call edited in that same commit. A scrape's region is a window a test cuts from the
  class's text through `std::string` (`find`, `substr`) or `QString` (`indexOf`,
  `mid`, `left`): an anchor plus a literal length, or the span between two
  anchors, located with the scrape's code as it stands at the parent. A
  window-cutting call in a class-reading test whose offsets the tool cannot
  evaluate is listed as unevaluated, and the commit's message names each one as
  checked by hand. *Breaks when:* a region
  silently loses its subject and a negative or count-based assertion stays green
  over it. *Test:* `tools/split-motion-check.py --scrapes --pre <parent> --post
  <cut> --stem <stem>` derives every such region from `tests/` at the parent,
  evaluates each over the class text at the parent and at the cut, and lists
  those whose content differs. It exits 0 when the cut commit also changes
  the window-cutting call of every listed scrape — not merely its file — and
  non-zero otherwise. The work-list is
  derived, never hard-coded, as ANTS-3833 INV-10 required.
- **INV-11** — `src/<stem>_internal.h` is included only by files in that
  class's source list. *Breaks when:* another subsystem takes a dependency on a
  helper that was never API. *Test:* `tests/features/split_sources` scans `src/`
  and `tests/` for the include, and checks the including files are a subset of
  the list.
- **INV-12** — Every function and type referenced across pieces has external
  linkage. Constants are outside it: a namespace-scope `const` links clean with
  internal linkage, so ANTS-3833 § 2.3's `extern` rule, checked when the
  promotion commit is reviewed, is their only guard.
  *Breaks when:* a promoted helper stays `static` or inside an anonymous
  namespace. *Test:* `ants-terminal` and every test bundle link. A static
  library's build alone proves nothing, per ANTS-3833 INV-7.

## 4. RAM / build cost

**No runtime cost.** No new state, no allocation, no dependency. Each piece joins
its class's existing library, so no build target is added.

**Compile cost moves three ways, as ANTS-3833 § 4 found.** The largest TU and the
cost of a one-feature edit fall. The CPU spent compiling the whole class rises,
because each piece pays the fixed per-TU cost. Peak memory of one compiler
process falls with the largest TU. **No figure is projected here.** Each item
measures its class before and after, with ANTS-3833 § 1's
`CCACHE_DISABLE=1 /usr/bin/time` command on the object file, and records both
figures in its commit message. Precompiled headers apply unchanged:
`ants_apply_qt_pch()` covers the three libraries' TUs, so a new piece reuses them.

## 5. Out of scope

- **Regrouping `tools/list` by family** — deferred by the user 2026-09-14;
  not yet queued.
- **ANTS-1049's data table** — deferred by the user 2026-09-14 to its own
  spec, after the splits; tracked by ANTS-1049.
- **Splitting any header** — permanent exclusion. The headers are the classes'
  APIs; INV-1 pins them.
- **Any change to behaviour, response envelopes, refusal codes, menus or the
  audit catalogue's contents** — permanent exclusion. INV-6 and INV-7
  hold that line; § 2.3's re-point of the `audit_fixture_coverage` command is the
  one authorised catalogue edit.
- **Moving `remotecontrol`'s `ANTS_RC_SOURCES` machinery onto
  `slurpSourceList`** — deferred; not yet queued. It works, and re-plumbing it
  inside these items adds blast radius for no gain here.
- **Turning the audit catalogue into data a user can edit** — permanent
  exclusion for these items, per CLAUDE.md § Key design decisions (non-obvious):
  hardcoded checks stay in C++.

## 6. Tests

| Test | Bundle | Covers | Kind |
|---|---|---|---|
| `tests/features/split_sources` | `test_core` — reads files only, like ANTS-3833's `rc_tu_split` | INV-2, INV-9, INV-11 | standing |
| `tests/features/mcp_tools_list_live` | `test_claude` — needs `ClaudeIntegration` | INV-6 | standing, plus migration-time dump |
| the audit readers case | `test_dialogs` — it constructs an `AuditDialog`, which the engine-only `test_audit` cannot link | INV-5 | standing |
| `tools/split-motion-check.py` | none — a script, shipped the way ANTS-3833 shipped `tools/rc-namespace-scan.py` | INV-7, INV-10 | migration-time |

Label `features;fast` for the two new feature directories. Each case added to a
bundle's `SOURCES`, never `add_executable`. Build that bundle and check
`ctest -N -R <name>` moved.

**Must fail first**, per project convention:

- `split_sources` — against a list missing one piece, against a stray
  `src/<stem>_x.cpp` outside the list, and against a file one line over the cap.
- `mcp_tools_list_live` — against a scratch build with one descriptor's
  description edited: the dump comparison differs. Against a scratch build whose
  `test_claude` definition of `ANTS_CLAUDEINTEGRATION_SOURCES` omits one piece
  holding tool descriptors, with the library's list unchanged: the standing name-set assertion fails.
- The audit readers case — against a scratch `audit_self_test.sh` whose
  extraction pattern matches nothing: the empty-set assertion fails.
- `split-motion-check.py` — against a commit pair with one moved line altered
  (INV-7), and against a cut where a scrape region's content changed but its window-cutting
  call did not (INV-10).

**INV-1, INV-3 and INV-4 are recorded commands** run at each cut. INV-12's
surface is the link every build already performs.

## 7. Cross-doc impact

| Document | Change |
|---|---|
| `.indie-review/partition.json` | each class's lane lists its new pieces; `tests/features/indie_review_partition_coverage` fails until it does |
| `docs/subsystems.md` | the lane entries for the three classes gain their pieces |
| `ROADMAP.md` | ANTS-1043 and ANTS-1044 quote stale line counts and the file names `auditcatalogue.cpp` and `auditexport.cpp`; annotate them. ANTS-1677 says *defer to 0.8.x*, overtaken by the user's 2026-09-14 ruling; annotate it |
| `CHANGELOG.md` | one `Changed` entry per item |
| `tests/audit_self_test.sh`, `tools/check-readme-claims.sh` | code changes under § 2.3, listed here because both are read as documentation of what they check |
| `README.md`, `PLUGINS.md`, `CLAUDE.md` | none — no user-visible change, no Lua surface change, and CLAUDE.md names none of the moved symbols |

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-09-14 | 3, cold — genre pinned `spec` | 1 | 5 | 2 | 3 | 11 findings after merging the three lanes (seven raised by all three); 11 verified, 0 dismissed; all 11 fixed. Q2: one kind per cut commit could not meet the 4,000-line cap (several cut commits now allowed, and § 3 says when each invariant applies); ANTS-1049 added a second `auditdialog` file before the machinery (the table is now built inside `src/auditdialog.cpp`); callable checks moved after the loop contradicted INV-8's order; INV-3 could never print 0 over the glob readers and `spec.md` prose (now `.cpp` only); `rcDelegate` captures `this` under two rules (now a private member). Q1: two of the four "silent" readers fail loudly. Q3: INV-5's id set per detected-type set (now the union); INV-7's marker was undefined. Q4: INV-10's zero-row condition was unmeetable; INV-4's negative test passed before the fix existed; INV-6's fail-first case could not fail. Verification also corrected the claim that `mcpOn` is shared across registrations. Three open questions resolved clean. Loop 2 dispatched. |
| 2 | 2026-09-14 | 3, cold — genre pinned `spec` | 0 | 5 | 3 | 4 | 12 findings after merging the three lanes; 12 verified, 0 dismissed; all 12 fixed. Q2: ANTS-1049's commit shape (a groundwork commit now carries INV-8's seam and the scrape check); § 2.2 and § 3 disagreed on which proofs each kind carries; INV-1 left no home for `rcDelegate`'s promotion or INV-8's seam; INV-8's byte identity against the re-pointed `audit_fixture_coverage` command; the promotion set could not be derived from a one-entry list (now from the seam plan). Q3: ANTS-1044 could not move a function-local table (rows now at namespace scope, conditions as data); INV-3 flagged path mentions that read nothing; INV-6's text-side name set caught non-tool `name` properties. Q4: INV-10 passed vacuously on the cut that migrates every reader (readers now migrate in groundwork, and the scrape call itself must change); INV-3's command re-derived an empty macro list; INV-6's must-fail case could not link; INV-5's runtime check prints only missing ids. Open questions resolved clean: no build-varying value in `tools/list`; `mainwindow.h` already includes `claudeintegration.h`; nested type gates are covered by INV-8's sets. **Capped at loop 2 (spec cap).** 8 of the 12 landed on text loop 1 wrote: a violent cap, so this document's review ends here and it routes to implementation. All 23 verified findings across the run fall inside the gated change, the whole file being new. Status set to accepted. |
| 3 | 2026-09-14 | 3, cold — genre pinned `spec`; first loop of the review of the split-first amendment | 2 | 2 | 2 | 4 | 10 findings after merging the three lanes; 10 verified, 0 dismissed; all 10 fixed. Q1: the macro inventory missed `MAINWINDOW_CPP` (no `SRC_` prefix); an anonymous-only namespace scan reads every seam as safe after promotion. Q2: re-pointing `audit_fixture_coverage` in the cut contradicted INV-7 (now re-pointed in groundwork, the one authorised catalogue edit); kind A and INV-7 named only `src/<stem>.cpp`. Q3: no entry point for the self-test's id extraction (now `--list-rule-ids`); INV-7 allowed no repeated preprocessor conditional. Q4: INV-6's name set ignored the `ANTS_LUA_PLUGINS` gate; INV-10 missed `QString` and computed windows and the promotion commit; INV-12 cannot catch a constant's linkage. Open questions resolved clean: the descriptor run reads no local of `onMcpConnection()` in code; INV-4 holds at every commit. Loop 2 dispatched. |
| 4 | 2026-09-14 | 3, cold — genre pinned `spec`; second and final loop of the amendment review | 1 | 2 | 2 | 4 | 9 findings after merging the three lanes; 9 verified, 0 dismissed; all 9 fixed. Q1: the audit readers case was sent to `test_audit`, which cannot link `AuditDialog` (now `test_dialogs`). Q2: INV-7 rejected a kind B carve kept in its own file; INV-6 skipped the promotion commits that rewrite descriptor lambdas. Q3: the ordinal marker's `<N>` was undefined; the list order read two ways and wrongly claimed ANTS-3833's guarantee. Q4: standing tests could land after the cuts they guard (checks now land first); nothing checked a compiled source list against the parsed one; a script cannot evaluate arbitrary computed windows (named forms, the rest listed); INV-6's must-fail case could pass. Open questions resolved clean: `serverInfo["name"]` does not match the tool pattern; INV-4 holds at every commit. **Capped at the spec cap.** 3 of the 9 landed on text this run's first loop wrote: a calm cap, so the spec ships to implementation. 3 of this run's 19 verified findings fall inside the amendment commit itself. Status set to accepted. |
