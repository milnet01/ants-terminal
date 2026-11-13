# ANTS-6245 — Finish a timed-out plain-text search through the git index

**Status:** implemented (2026-11-13).
**Kind:** perf.
**Source:** ROADMAP ANTS-6245 (ANTS-5323 follow-up 2026-10-24; five peer reports).
**Composes with:** ANTS-1248 (`workspace_search`), ANTS-1565 (its wall budget), ANTS-5323 (`truncated_reason`).

**Layman:** A plain-text code search that runs out of time on a cold disk now finishes by a faster route instead of returning half an answer.

## 1. Goal

After this ships, a literal `workspace_search` that hits its wall budget on a
git tree runs a second, narrowed scan and returns the complete answer. The
second scan skips only files that git has proved cannot hold the text. A
regex search, and any search where that proof is unavailable, replies exactly
as it does today.

## 2. Problem

`rcRunRg` (`src/remotecontrol_workspace.cpp`) stops ripgrep at the wall
budget. `RemoteControl::cmdWorkspaceSearch` then returns partial rows with
`truncated_reason:"timeout"`, or `rg_failed` when nothing was parsed.

The cause is seeks for cold file data on a rotational disk, not the scan
itself. Measured on this repository on 2026-11-13 (ripgrep 15.2.0, git
2.56.0, file data and `.git` evicted with `posix_fadvise(DONTNEED)`, literal
`cmdWorkspaceSearch`, whole tree):

| Step | Cold | Warm |
|------|------|------|
| `rg --json -F --smart-case <literal> .` | 16.7 s | 28 ms |
| `git ls-files -z -t -v -c -m -s` | 32 ms | 9 ms |
| `LC_ALL=C git grep --cached -l -z -i -F -f <needles>` | 1.5 s | 1.2 s |
| the same `rg` with the skip list of § 4.4 | 0.6 s | 31 ms |

The index read comes from packfiles, which are few and contiguous. So the
narrowed route costs about 2 s where the full scan costs 17 s.

A prefilter can drop a match without any error. Each case below was
reproduced on a throwaway repository the same day; § 4 says how the design
avoids it.

1. ripgrep `-i` folds Unicode: `kelvin` matches `K` (U+212A) and `test`
   matches `teſt` (U+017F). `LC_ALL=C git grep -i` matches neither.
2. ripgrep transcodes a UTF-16 file that starts with a byte-order mark and
   finds text in it. `git grep` sees the raw bytes and does not.
3. ripgrep handed a file by name searches a binary file it would skip
   during a walk, and ignores `--glob` for that file. So naming the
   candidate files changes the answer.
4. A file flagged `assume-unchanged` or `skip-worktree` is not reported by
   `git ls-files -m` when its working copy differs from the index.
5. A `filter`, `ident` or `working-tree-encoding` attribute makes the working
   copy differ from the index blob with no modification to report.

## 3. Scope decisions (agreed with the user)

- **Literals only; a regex stays as today; the default budget does not
  grow.** User, 2026-11-13, recorded on the roadmap item.
- **The fallback runs after a timeout, never first.** Author's call: the warm
  path stays one ripgrep run, and the index read costs more than a warm scan.
- **Narrow by exclusion, not by naming files.** Author's call, from § 2
  item 3.

## 4. Surface

### 4.1 When the fallback runs

All of these hold, checked in this order. The first that fails gives the
`timeout_fallback_skipped` value shown, and the reply is otherwise today's.

| Condition | Skipped value |
|-----------|---------------|
| The first scan ended with `RgRun::hardKilled`. A stop at the output ceiling or the parse budget does not count. | *(no field)* |
| The pattern is a literal: `regex` false and `match_wrapped` false. | `regex` |
| The pattern holds no byte below 0x20. | `control_char` |
| The fallback budget (§ 4.5) is at least one second. | `budget` |
| The needle (§ 4.2) is at least three bytes long. | `short_needle` |
| Every git step (§ 4.3) exits zero inside the budget. | `git_failed` |
| The skip list (§ 4.4) is not empty. | `no_gain` |
| The second scan ends without a hard kill, an output-ceiling stop, a parse-budget stop, a crash or an exit code of two or more. | `scan_failed` |

`respect_gitignore`, `include_hidden`, `glob`, `exclude_glob`, `lane`,
`context` and every reply mode need no condition. The second scan passes the
same arguments, and the skip list only removes files.

### 4.2 The needle

The needle is the longest run of pattern bytes that are printable ASCII and
are none of `k`, `K`, `s`, `S`. The first such run wins a tie. git matches it
without regard to ASCII case, under `LC_ALL=C`.

This holds for every `case` mode. Any text ripgrep accepts as a match of the
whole pattern contains the needle under ASCII folding, because the excluded
letters are the only ASCII letters whose Unicode fold class reaches outside
ASCII (§ 2 item 1).

### 4.3 The git steps

Each runs from the project root with `GitWrap::readOnlyEnvironment()` plus
`LC_ALL=C`, shell-less. When `lane` is set, its root-relative path is the
pathspec of the first two.

```
git ls-files -z -t -v -c -m -s [-- <lane>]
git grep --cached -l -z -i -F -f <needle file> [-- <lane>]
git ls-files -z -- ':(exclude,attr:!filter !ident !working-tree-encoding)'
```

The needle file holds three fixed strings, one per line: the needle, the
bytes `FF FE`, and the bytes `FE FF`. The last two list every file that
could start with a UTF-16 byte-order mark (§ 2 item 2). They go through a
file because argv cannot carry them.

### 4.4 The skip list

A path joins the skip list only when all of these hold:

- the first step lists it once, with tag `H`, stage `0` and mode `100644` or
  `100755`;
- the second step does not list it;
- the third step does not list it.

A second row with tag `C` marks a modified file. A lower-case tag or `S`
marks § 2 item 4. Any other tag, stage or mode keeps the file in the scan.
Untracked files and files inside a submodule never appear in the first step,
so they are always scanned.

Each skipped path becomes one ripgrep argument pair, appended after every
other glob:

```
--glob '!/<path with each of [ ] * ? { } \ escaped by a backslash>'
```

Paths are taken in the first step's order until the pairs total one
mebibyte. The rest stay in the scan. A partial skip list is still correct.

The second scan goes through `rcRunRg`, so ANTS-3716's single call site
stands. It fills a fresh set of the accumulators `onLine` writes. The first
scan's set is kept and is what a skipped fallback reports.

### 4.5 Budgets

```
fallback budget = min(budget, 30 s − budget)
```

It covers the git steps and the second scan together. The first scan plus the
fallback therefore never outlast `kWorkspaceSearchMaxBudgetMs`, and
ANTS-1565 INV-5's margin is unchanged. At the default the fallback gets five
seconds.

### 4.6 The reply

A fallback that completes replies as an uncut scan does, with one added
field. `truncated` and `truncated_reason` then describe only a row or byte
cap.

```json
{ "timeout_fallback": "git_index" }
```

A timeout reply whose fallback did not complete carries the reason, on the
`ok:true` partial reply and on the `rg_failed` refusal alike:

```json
{ "timeout_fallback_skipped": "<a value from § 4.1>" }
```

`elapsed_ms` covers both scans. The timeout hint from
`rcStampTruncatedReason` is unchanged.

### 4.7 Code layout

`src/searchprefilter.{h,cpp}` holds the pure parts: the needle, the parse of
the three git outputs into a skip list, and the glob escape. It includes no
process or file class. `cmdWorkspaceSearch` runs the processes.

`RemoteControl::setSearchTimeoutForced(bool)` is a test-only seam, like
`setRgStdoutCapOverride`. When set, the first scan is not run and is treated
as hard-killed with nothing parsed.

## 5. Invariants

Every test below lives in
`tests/features/workspace_search_timeout_fallback/`. Each builds a git
repository in a temporary directory and compares a forced-timeout reply with
a plain reply to the same request.

- **INV-1** — A completed fallback returns the same match rows, in any
  order, as the plain scan, and carries `timeout_fallback:"git_index"`.
  *Test:* `test_workspace_search_timeout_fallback.cpp`, case `sameRowsAsPlainScan`. Isolates § 4.4's "second step does not list it": the fixture has files with and without the text.
  *Breaks when:* the skip list holds a file that contains the pattern.

- **INV-2** — A file whose only match needs Unicode case folding is found.
  *Test:* case `unicodeFoldKept`, patterns `kelvin` and `xyztest` against lines holding `Kelvin` (opening with U+212A) and `xyzteſt`. Plain `test` has the two-byte needle `te`, which § 4.1 skips. Isolates § 4.2's excluded letters: with them allowed in the needle, git drops both files.
  *Breaks when:* the needle contains `k` or `s`, or git runs without `LC_ALL=C`.

- **INV-3** — A tracked UTF-16 file with a byte-order mark is found.
  *Test:* case `utf16Kept`. Isolates the two byte-order-mark lines of the needle file.
  *Breaks when:* the needle file holds the needle alone.

- **INV-4** — A match in a modified, `assume-unchanged`, `skip-worktree`,
  untracked or unmerged file is found.
  *Test:* case `dirtyStatesKept`, one file per state, each matching only in its working copy. Isolates § 4.4's tag, stage and row-count rule.
  *Breaks when:* the skip list is built from the grep output without the first git step.

- **INV-5** — A match in a file carrying a `filter`, `ident` or
  `working-tree-encoding` attribute is found.
  *Test:* case `attributedKept`, an `ident` file matched on its expanded `$Id` text. Isolates the third git step.
  *Breaks when:* that step is dropped.

- **INV-6** — The fallback reports no row the plain scan does not. A tracked
  binary file holding the text, and a file outside `glob`, stay out.
  *Test:* case `noExtraRows`. Isolates § 3's exclusion route: naming files to ripgrep adds both.
  *Breaks when:* the second scan names candidate files instead of excluding the rest.

- **INV-7** — A path holding `[`, `]`, `*`, `?`, `{`, `}`, `\`, a space, or a
  leading `!` or `#` is skipped when it lacks the text and scanned when it
  holds it.
  *Test:* `test_searchprefilter.cpp`, case `globEscape`, plus `oddNamesRoundTrip` in the feature test, which asserts the rows and that the odd-named files without the text were excluded from the argv.
  *Breaks when:* a metacharacter is passed through unescaped.

- **INV-8** — A regex, a `match_wrapped` search, a needle under three bytes,
  a folder that is not a git tree, and a budget of 30 s each reply as today
  with the matching `timeout_fallback_skipped` value.
  *Test:* case `skippedReasons`, one request per row of § 4.1.
  *Breaks when:* any condition is not checked, or a skipped fallback changes another reply field.

- **INV-9** — The project root may sit below the git top level, and `lane`
  may narrow the search; INV-1 holds in both.
  *Test:* case `subdirRootAndLane`. Isolates the root-relative paths of § 4.3 and § 4.4: paths taken relative to the git top level skip nothing or the wrong file.
  *Breaks when:* git runs from the git top level rather than the project root.

- **INV-10** — The first scan plus the fallback never run longer than
  `kWorkspaceSearchMaxBudgetMs` plus the kill graces.
  *Test:* `test_searchprefilter.cpp`, case `fallbackBudget`, over every `timeout_sec` from 1 to 30.
  *Breaks when:* the fallback is given the full budget again at `timeout_sec` above 15.

- **INV-11** — The pattern reaches git only as file content, and every
  process is started shell-less with an argv list (ANTS-1248 INV-3).
  *Test:* case `patternNotInArgv`, a pattern starting with `-` and one holding a quote, each found; plus a source scrape for a shell or single-string start in `src/searchprefilter.cpp` and the fallback block.
  *Breaks when:* the needle is passed as `-e <needle>` or through a shell.

## 6. Failure modes

- **git missing, not a repository, or a step times out** → `git_failed`;
  today's reply.
- **The index changes between the git steps and the second scan** → a file
  edited in that window may be skipped on its old content. The same window
  exists between any two tool calls; not defended.
- **A repository with more paths than the mebibyte cap admits** → the rest
  are scanned; the second scan may time out → `scan_failed`.
- **A clean or smudge filter set outside `.gitattributes`** → covered: the
  third step reads attributes from every source git does.
- **A pattern common to most files** → the skip list is short and the second
  scan may time out → `scan_failed`.
- **The second scan is cut by the output ceiling or parse budget** →
  `scan_failed`; the first scan's partial reply stands.

## 7. Tests

| Invariant | Test |
|-----------|------|
| INV-1 to INV-6, INV-8, INV-9, INV-11 | `tests/features/workspace_search_timeout_fallback/test_workspace_search_timeout_fallback.cpp` |
| INV-7 | that file, and `tests/features/workspace_search_timeout_fallback/test_searchprefilter.cpp` |
| INV-10 | `test_searchprefilter.cpp` |

Label `features;fast`. Each feature case must be seen failing first: against a build where the seam
exists and the fallback does not, the forced-timeout reply is `rg_failed`.
The tests prove the answer is the same; they do not measure speed. § 2's
timings need an evicted disk cache and are not reproduced by any test.

## 8. Alternatives considered (and rejected)

- **Name the candidate files to ripgrep.** Rejected: § 2 item 3 changes the
  answer for binary files and globs.
- **A skip list through `--ignore-file`.** Rejected: measured, a positive
  `--glob` and a `.gitignore` negation both override it, so a search with
  `glob` gains nothing.
- **Run the prefilter before the first scan.** Rejected: 1.2 s warm against a
  28 ms scan.
- **`git grep` as the search itself.** Rejected: it cannot see untracked
  files and reports binary files differently.
- **Prefilter a regex.** Rejected by the user; git's regex dialects differ
  from ripgrep's.
- **`git ls-files --eol` to find UTF-16 files.** Rejected: a second read of
  every blob, where the byte-order-mark needles ride the read already made.

## 9. Out of scope

- A longer default budget — rejected by the user, 2026-11-13.
- Serving a worktree from the index, and reporting files walked against the
  total — deferred; not yet queued.
- `cited_by`, the other `rcRunRg` caller — deferred; not yet queued.

## 10. What checks this

| Rule | What catches a breach |
|------|----------------------|
| INV-1 to INV-9, INV-11 | the feature test's cases named in § 5 |
| INV-10 | `test_searchprefilter.cpp::fallbackBudget` |
| `src/searchprefilter.cpp` stays free of process and file classes | **nothing** — review only |
| § 2's timings still hold | **nothing** — needs an evicted disk cache |
| A hazard not listed in § 2 | **nothing** — the tests cover the five found |

## 11. Cross-doc impact

- `CHANGELOG.md` — one line under `[Unreleased]`.
- `docs/standards/mcp-behavioural-notes.md` — the two reply fields.
- The `workspace_search` tool description, if the timeout hint is reworded.
- ANTS-1565 and ANTS-1248 — unchanged; their clauses stand.

## 12. Cold-eyes loop log

Rows live in `../reviews/ANTS-6245-search-timeout-git-fallback-loop-log.md`.

## 13. Reload

`workspace_search` is registered in `mcp::registerProjectScopedVerbs()`, so
it runs in the `ants-mcpd` process. A rebuilt `ants-mcpd` serves this on a
client's next reconnect, with no terminal relaunch. It holds no state between
calls, so it has no stop or start control.

## 14. RAM / build cost

Up to three git processes and one more ripgrep per timed-out literal search,
run one after another. One temporary needle file, removed before the reply.
The skip arguments are capped at one mebibyte. No state is kept between
calls and no dependency is added; git is already required by `git_state`.
