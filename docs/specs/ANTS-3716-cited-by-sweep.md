# ANTS-3716 — `cited_by`: one pass that says which document cites which anchor

**Status:** spec draft (2026-08-14).
**Kind:** feature.
**Source:** ROADMAP.md ANTS-3716 (claude-config-feedback-2026-07-28; the
claude_config session, reporting on its own review orchestrator).
**Pairs with:** ANTS-3715.

## 1. Problem

After a batch of fixes lands, both `/apply-fixes` and `review-contract` run the
same sweep: for every symbol, flag, config key or path the batch changed, find
every document that names it, and record a verdict for each. `/apply-fixes`
§ 4's own table gives the recipe — *"`workspace_search` the **old** name over
`docs/`, `README.md`, `CLAUDE.md`; `files_only` first"* — once per changed
anchor.

Three consequences, in the order they cost:

1. **It is N round-trips of reasoning, not one search.** The reporting session
   called it *"a PURE SEARCH … being performed as reasoning"*. The ledger is
   consumed four times per loop by design, so the cost multiplies with loop
   count and none of it exercises judgement.
2. **The batched form loses the answer.** A caller who avoids the N calls by
   passing one regex alternation gets `workspace_search`'s `files_only` shape —
   `{files:[{file, count}], files_count, count}` — which says *a* document
   matched *something*. It cannot say which anchor that document cites, and the
   anchor is what tells the reader what to check when they open it.
3. **The unit of work is the cell, and nothing produces cells.**
   `/apply-fixes` § 4 requires *"a verdict per cell, not a list of targets"*,
   with four verdicts (`agrees` / `fixed` / `surfaced` / `frozen`), and states
   the failure directly: *"A cell you did not open has no verdict, and a sweep
   with unfilled verdicts is not finished."* A cell is an (anchor, document)
   pair. A file list is not a grid, so the caller reconstructs the grid by
   re-searching per anchor — which is consequence 1 again.

The measurement behind the ask: of nine HIGH findings in one loop, **five had
been introduced by earlier loops' own fixes**, and the ledger was being kept
throughout. What was missing was the per-cell verdict that would have shown the
sweep never reached them.

This spec covers the `sweep` half of ANTS-3716 only. The bullet also proposed
`open` / `close` / `report` ops over a stored ledger; § 5 records why they are
not here.

## 2. Surface

### 2.1 The verb

A new read-only verb `cited_by`, registered like its siblings — schema in
`src/claudeintegration.cpp`, delegate in `src/mainwindow.cpp` via `rcDelegate`,
handler `RemoteControl::cmdCitedBy` in `src/remotecontrol_workspace.cpp` beside
`cmdWorkspaceSearch`, which it reuses the machinery of.

```cpp
// src/remotecontrol.h
QJsonDocument cmdCitedBy(const QJsonObject &req);
```

Request:

```json
{
  "caller_cwd": "/path/to/project",
  "anchors":    ["oldFunctionName", "--old-flag", "src/foo.cpp"],
  "scope":      ["docs", "README.md", "CLAUDE.md"],
  "case":       "insensitive",
  "max_cells":  500
}
```

- **`anchors`** (required, 1–64 strings) — the changed names. **Literals, never
  regexes**: an anchor is a symbol, flag, config key, env var or path, and
  `workspace_search` already owns the regex case. They reach rg **unescaped,
  under `--fixed-strings`** — the same way `cmdWorkspaceSearch` handles its own
  literal case (`argv << "--fixed-strings"` when `!isRegex`). Empty array or
  > 64 entries refuses `bad_args`.
- **`scope`** (optional, project-relative files and/or dirs) — defaults to the
  three targets `/apply-fixes` § 4 names: the project's `docs_dir` (via
  `ProjectSettings::load(rootCanonical).docsDir`, else `docs`), `README.md` and
  `CLAUDE.md`. Every entry runs through `PathValidation::validatePath`; a
  root-escaping entry refuses `bad_path`. **A scope entry that does not exist on
  disk is pruned before argv is built** — see § 2.5.
- **`case`** (optional) — `insensitive` (default) or `sensitive`. **There is no
  `smart` mode**; § 2.2 gives the measurement that rules it out.
- **`max_cells`** (optional, default 500, clamp 1–5000).
- **`timeout_sec`** (optional, default 5, clamp 1–30) — the rg wall-clock
  budget, same shape and default as `workspace_search`'s.

**Anchors are not escaped, and that is a correctness requirement rather than a
simplification.** `QRegularExpression::escape` backslashes every character
outside `[A-Za-z0-9_]`, non-ASCII included, and rg's Rust regex engine rejects a
backslash before a non-ASCII character. Measured on the installed ripgrep
15.2.0:

```
$ rg --json -e 'caf\é' esc.txt
rg: regex parse error:   error: unrecognized escape sequence     → exit 2
```

Every anchor shares one argv (§ 2.3), so a single accented anchor — and this
project handles accented paths deliberately (`pathvalidation.cpp`, ANTS-1837) —
would abort the whole pass and return every anchor as uncited, which is exactly
the silent drop § 2.4 exists to prevent. `--fixed-strings` with raw anchors has
no such failure mode and satisfies INV-3 on its own: measured, `rg -F -e
'foo.cpp'` matches `foo.cpp` and not `fooXcpp`.

### 2.2 Case: `insensitive` by default, and no `smart` mode at all

`workspace_search` defaults to `smart` and offers all three. This verb defaults
to **`insensitive`** and **does not offer `smart`**. Both departures are
deliberate and neither is cosmetic.

**Why `insensitive` is the default.** A cell the sweep never produced is a
document nobody opens, which is § 1's documented failure; a cell that turns out
to be a coincidental match costs one `agrees` verdict. The two errors are not
symmetric. `/write-code`'s trigger table records the measurement — of a changed
UI string, *"a case-sensitive grep … missed the test asserting it
`.lower()`-ed"*.

**Why `smart` is absent, which is the load-bearing half.** rg resolves
`--smart-case` over the **combined** pattern set, not per `-e`. Measured on
ripgrep 15.2.0 against a file holding `FOO here` and `bar here`:

```
$ rg --smart-case -e foo -e bar   → FOO here / bar here   (exit 0)
$ rg --smart-case -e foo -e Bar   → (nothing)             (exit 1)
```

So one anchor containing a capital — `oldName`, `README.md`, `cmdCitedBy`, all
realistic — silently flips every *other* anchor in the same request to
case-sensitive, and the cells they would have produced are never emitted for
attribution to recover. The same anchor over the same tree would return
different cells depending on which batch it was submitted with. A sweep whose
results depend on the company an anchor keeps is worse than no sweep, because
it looks complete. Offering the mode and documenting the coupling was the
alternative; it was rejected because no caller can act on that knowledge.

### 2.3 One pass, with attribution

The whole point is one traversal of the doc set that still attributes each hit
to its anchor. rg takes repeated `-e` patterns, so:

1. Build one argv with `-e <anchor>` per anchor, plus `--fixed-strings`,
   `--json`, the `case` flag, and the resolved scope paths.
2. Run it once.
3. Attribute from each match event's **`submatches[]`**, not by re-testing the
   line. Under `--fixed-strings` every submatch's `match.text` *is* the anchor
   that produced it, so attribution is exact by construction.

Measured on ripgrep 15.2.0 — `rg --json -F -e alpha -e beta` over the line
`alpha and beta and alpha again` yields one match event whose submatches are:

```
[('alpha', 0), ('beta', 10), ('alpha', 19)]
```

Every occurrence, with its matched text and byte offset. Two consequences the
first draft of this spec got wrong: a match event does **not** merely carry the
line, and attribution needs no substring test — which also removes the
nested-anchor hazard (`foo` inside `foobar`) a whole-line test would have had.

A line citing two anchors yields **two cells**, which is correct: it needs a
verdict against each.

### 2.4 The rg helper, and what it must own

rg is invoked as `cmdWorkspaceSearch` does today —
`QProcess::start(QString, QStringList)` with the argv list, no shell
(`src/remotecontrol_workspace.cpp:309`–`:314`, the ANTS-1248 INV-3 contract).

**The extraction is a run-to-completion helper, not a move of the six lines
around `rg.start`.** `cmdWorkspaceSearch` keeps the process live well past that
window — `waitForStarted(500)`, the `timeout_sec` hard kill with its
terminate/kill grace, the stderr cap, `readAllStandardOutput()`, and the
exit-status classification at `src/remotecontrol_workspace.cpp:505` that turns
a crash, an `exitCode() >= 2`, a hard kill or a parse-budget overrun into
`rg_failed`. A helper owning only the start leaves `QProcess` in both handlers
and, worse, leaves `cited_by` without the exit-code branch — which is the
difference between a refusal and `ok:true` with every anchor wrongly reported
uncited.

So the helper takes the argv and the budget and returns the completed run —
stdout bytes, stderr tail, exit code, exit status, and whether it was hard
killed — and each handler renders its own refusal envelope from that, with its
own verb name in the message. `cmdWorkspaceSearch`'s `wsErr("rg_failed", …)`
strings name `workspace-search` and must not be reused verbatim for this verb.

### 2.5 A missing scope path is pruned, not passed to rg

rg exits **2** when handed a path that does not exist, and the classifier above
turns that into a failure. Measured:

```
$ rg --json -e alpha -- b.txt /nonexistent-xyz
rg: /nonexistent-xyz: No such file or directory (os error 2)   → exit 2
```

The default scope names `README.md` and `CLAUDE.md`, which not every project
has. Left unhandled, the default call would refuse `rg_failed` on any such
project when no anchor matched, and return partial results with a swallowed
error when one did.

`PathValidation::validatePath` does not require existence, so it will not catch
this. Therefore: **every scope entry, defaulted or caller-supplied, is stat'd
after validation and dropped if absent**; `scope_resolved` echoes the surviving
set, so a caller can see what was actually searched. A `scope` that prunes to
empty returns `ok:true` with no cells and every anchor in `anchors_unmatched` —
correct, and visible through `scope_resolved: []`.

### 2.6 Response

```json
{
  "ok": true,
  "cells": [
    {"anchor": "oldFunctionName", "file": "docs/specs/ANTS-1.md", "count": 3, "first_line": 42}
  ],
  "cells_count": 1,
  "anchors_matched":   ["oldFunctionName"],
  "anchors_unmatched": ["--old-flag", "src/foo.cpp"],
  "files_count": 1,
  "scope_resolved": ["docs", "README.md", "CLAUDE.md"],
  "truncated": false
}
```

- **`count` is OCCURRENCES, not matching lines** — the total length of that
  pair's `submatches[]` across the file. An anchor named twice on one line
  counts 2. Stated because the two readings are one `++` apart and the field
  name does not disambiguate them.
- **`first_line` is the lowest 1-based line number** for that pair.
- **`cells` is sorted by (anchor, file); `anchors_matched` and
  `anchors_unmatched` are in request order; `scope_resolved` is in resolution
  order.** Every array has a stated order because rg's does not: measured, six
  identical runs of the reused invocation over `docs/specs README.md CLAUDE.md`
  produced **six different event orderings**, because `cmdWorkspaceSearch`
  passes `--threads 4` and rg does not order output across files in parallel.
  An array built in arrival order would vary run to run.
- **The cap is applied AFTER the sort, never during the stream.** Truncating
  while parsing would retain a run-dependent subset — sorting the survivors
  cannot restore byte-identity — and would also let an anchor first cited past
  the cap fall into `anchors_unmatched`, reporting "nothing cites this" when
  something does. So: consume the whole stream, tally every cell, sort,
  truncate to `max_cells`. `anchors_matched` / `anchors_unmatched` are therefore
  always computed over the **full** stream and are meaningful even when
  `truncated` is true.
- **`cells_count` is the capped array's length; `files_count` is uncapped** —
  the number of distinct files that matched across the whole stream. A cap that
  reads as completeness is the defect this shape exists to avoid.
- **`anchors_unmatched` is a first-class result, not an omission.** "Nothing
  cites this" is the cheap half of the sweep and the caller needs it stated;
  inferring it from an absence is how a caller silently drops an anchor it
  mistyped.
- No line bodies. The caller opens the file to record a verdict regardless, and
  `first_line` is enough to get there. `read_region` is the drill-in.

ETag-supported (`isEtagSupportedTool`) like the other read verbs — which the
stated array orders are a precondition for, since the ETag is a content hash of
the envelope and an unstable order would never 304.

## 3. Invariants

- **INV-1** — Every (anchor, file) pair with at least one match appears as
  exactly one cell, whose `count` is that pair's **occurrence** total (the
  summed length of its `submatches[]`), not its matching-line total. Two anchors
  on one line produce two cells. *Test:* `tests/features/cited_by/` — a fixture
  doc citing two anchors on one line, one anchor twice on separate lines, and
  one anchor **twice on a single line** → 4 cells with counts 1, 1, 2, 2; the
  last case is what distinguishes occurrences from lines and fails a per-line
  tally.
- **INV-2** — An anchor with no match anywhere in scope appears in
  `anchors_unmatched` and in no cell; an anchor with a match appears in
  `anchors_matched` and never in `anchors_unmatched`. The two arrays partition
  `anchors` exactly, **in request order, and are computed over the full stream
  even when `truncated` is true**. *Test:* `tests/features/cited_by/` — fixture
  with one cited and one uncited anchor → the arrays partition and their union
  equals the request's `anchors`; then re-run with `max_cells:1` over a fixture
  whose second anchor's only citation sorts past the cap → that anchor is still
  in `anchors_matched`.
- **INV-3** — An anchor is matched literally and is passed to rg **unescaped
  under `--fixed-strings`**: a regex metacharacter matches itself, and a
  non-ASCII character does not abort the run. *Test:*
  `tests/features/cited_by/` — anchor `foo.cpp` against a doc containing
  `fooXcpp` and `foo.cpp` → one cell, `count` 1; and an anchor containing `é`
  against a doc citing it → one cell, `ok:true` (this case exits 2 with a regex
  parse error under any escaping layer, so it fails against the escaped design).
- **INV-4** — `case` accepts `insensitive` (default) and `sensitive` and
  **nothing else**; `smart` refuses `bad_args`. *Test:*
  `tests/features/cited_by/` — anchor `oldName` against a doc writing `oldname`
  → one cell with no `case` in the request, zero cells with `case:"sensitive"`,
  and `case:"smart"` → `bad_args`.
- **INV-5** — `scope` omitted resolves to the project's `docs_dir` override
  (else `docs`), `README.md` and `CLAUDE.md`, and `scope_resolved` echoes what
  was searched. *Test:* `tests/features/cited_by/` — fixture with a match in
  `docs/`, one in `README.md` and one in `other/x.md` → the first two appear,
  the third does not.
- **INV-6** — A root-escaping `scope` entry refuses `code:"bad_path"` and
  searches nothing; `anchors` absent, empty, or over 64 entries refuses
  `code:"bad_args"`. Neither refusal runs rg. *Test:*
  `tests/features/cited_by/` — `scope:["../outside"]` → `bad_path`;
  `anchors:[]` → `bad_args`.
- **INV-7** — `cells` is sorted by (anchor, file) and the cap is applied
  **after** that sort, so two runs over an unchanged tree return byte-identical
  bodies even when truncated. *Test:* `tests/features/cited_by/` — a fixture
  **spanning several files**, whose rg traversal order differs from (anchor,
  file) order (anchor `zeta` cited in `docs/a.md`, anchor `alpha` in
  `docs/z.md`), asserting `cells[0].anchor == "alpha"` — an arrival-order build
  fails this. Then the same fixture with `max_cells` below the cell total, run
  twice, comparing serialized envelopes. The two-run comparison alone cannot
  fail an unsorted build, which is why the explicit ordering assertion carries
  the invariant.
- **INV-8** — At `max_cells` the response carries `truncated:true`,
  `cells_count` reports the **capped** array length, and `files_count` reports
  the **uncapped** number of distinct matching files. A cap that reads as
  completeness is the defect this names. *Test:*
  `tests/features/cited_by/` — fixture producing 5 cells spread over **4
  distinct files**, called with `max_cells:2` → `cells.size()==2`,
  `cells_count==2`, `truncated:true`, **and `files_count==4`**. The
  `files_count` assertion is the one that fails the natural build, which derives
  it from the capped array.
- **INV-9** — One rg call site exists in `src/remotecontrol_workspace.cpp`, in a
  helper that owns the process end to end — start, budget, hard kill, stderr
  cap, and exit-status classification — and neither `cmdWorkspaceSearch` nor
  `cmdCitedBy` names `QProcess` directly. *Test:* source scrape over that file
  asserting exactly one `rg.start(` and zero `QProcess` mentions inside either
  handler's body.
- **INV-10** — An rg run that fails (crash, `exitCode() >= 2`, hard kill, parse
  budget) refuses `code:"rg_failed"` naming `cited_by`, and never returns
  `ok:true` with anchors reported uncited. *Test:*
  `tests/features/cited_by/` — force a failure via a `timeout_sec:1` run over a
  fixture large enough to exceed it → `ok:false`, `code:"rg_failed"`, and the
  message names `cited_by` rather than `workspace-search`.
- **INV-11** — A scope entry that does not exist on disk is pruned before rg is
  invoked; `scope_resolved` echoes only the surviving entries, and a scope
  pruning to empty returns `ok:true` with `scope_resolved:[]` and every anchor
  in `anchors_unmatched`. *Test:* `tests/features/cited_by/` — default scope
  against a fixture project with **no** `CLAUDE.md` → `ok:true` (not
  `rg_failed`), `scope_resolved` omits it; and `scope:["nope"]` → `ok:true`,
  `scope_resolved:[]`.

## 4. RAM / build cost

No new library, no new build target, no on-disk state — the verb is a pure
function of (tree, request) and writes nothing, so it needs no cache-key or
relocation contract.

**`max_cells` does NOT bound peak memory**, and saying it did was wrong: § 2.6
requires the whole stream to be tallied before the sort and the cap, precisely
so an anchor cited past the cap is not misreported as uncited. The live bound is
therefore the number of **distinct (anchor, file) pairs**, which is at most
`anchors` × files-in-scope — 64 × a few thousand documents in the worst
realistic case, at ~120 bytes per cell, so single-digit MB. rg's `--json` output
is still consumed per event and discarded, never buffered whole; only the tally
survives.

A hard ceiling of **50,000 collected cells** guards the pathological case: past
it, collection stops, the response is truncated as usual, and `files_count`
reports what was seen — the one place the uncapped-counter promise degrades, and
it is 100× the default `max_cells`. `anchors` is capped at 64 so the argv cannot
grow unbounded.

## 5. Out of scope

- **The `open` / `close` / `report` ops of ANTS-3716's proposed `fix_ledger`
  verb.** A permanent exclusion for this spec, not deferred work. The bullet's
  own maintainer note asked that ledger rows be keyed on the finding with a
  disposition column; `review-contract` now does exactly that — one row per
  verified finding, in that skill's own `SKILL.md`, which lives under
  `~/.claude/skills/` and is outside this repository, so it is named rather
  than cited as a resolvable path. The row-shape problem is therefore closed
  and the storage half has no remaining customer. If one appears it is a new
  item, filed then.
- **Verdict storage.** The four verdicts are judgement and stay with the caller;
  this verb produces the grid they are recorded against. Permanent — a verb that
  stored verdicts would be storing the one part of the sweep that is not
  mechanical.
- **Regex anchors.** Permanent: attribution in § 2.3 is a literal substring
  test, and `workspace_search` already serves the regex case.
- **Cross-run memory** — whether a rejection survives into a later run is
  ANTS-3715's remaining half, re-scoped 2026-08-14. Deferred, tracked there.

## 6. Tests

`tests/features/cited_by/`, label `features;fast`, following the bundle
convention in `tests/features/README.md` — the source joins an existing bundle's
`SOURCES` list rather than getting its own `add_executable`, and the bundle is
identified by reading upward from the `grep -n cited_by CMakeLists.txt` hit to
the enclosing `ants_add_*_bundle(`.

`spec.md` beside it maps INV-1…INV-11 to cases. Per the project convention each
case is verified to **fail against pre-fix code** before the implementation is
restored; INV-9's source scrape is the one case that cannot (it asserts a refit,
so it fails by construction until the extraction lands).

Four cases are specified to defeat a vacuous pass, each named in its invariant
because the natural implementation passes the weaker version: INV-1's
twice-on-one-line anchor (separates occurrences from lines), INV-7's explicit
`cells[0]` assertion (a two-run comparison alone cannot fail an arrival-order
build, since rg is deterministic over a fixed tree), INV-8's `files_count==4`
(the natural build derives it from the capped array), and INV-3's accented
anchor (passes trivially unless an escaping layer is present). INV-6's and
INV-10's refusals each pair with a positive control in the same test, since a
handler that refuses everything satisfies a refusal assertion alone.

## 7. Cross-doc impact

- `docs/standards/mcp-tools.md` — no change; the verb follows the existing
  response-wrap, `caller_cwd`, path-validation and ETag contracts as written.
- `docs/standards/mcp-error-codes.md` — no new code. `bad_args`, `bad_path` and
  `rg_failed` are all existing; `rg_failed` is `workspace_search`'s and this
  verb becomes its second emitter, so the taxonomy entry needs `cited_by` added
  to the list of verbs that raise it.
- `CLAUDE.md` — no change. The module map lives in `docs/subsystems.md`; the
  `remotecontrol` lane entry already covers `remotecontrol_workspace.cpp` and
  names no verb inventory, so nothing there goes stale.
- `CHANGELOG.md` — one `Added` entry on ship.
- The consumer skills (`review-contract`, `/apply-fixes`) live in `~/.claude`
  and are **not** this project's to edit. Ship the verb; the asking session
  adopts it.

## Cold-eyes loop log

| Loop | Date | Findings | Verified | Dismissed | Fixed | Notes |
|---|---|---|---|---|---|---|
| 1 | 2026-08-14 | 14 raw / 9 distinct | 9 | 0 | 9 | 3 cold lanes, `general-purpose`. **Q1 ×3 · Q2 ×2 · Q3 ×3 · Q4 ×1.** Four empirical claims re-run by the orchestrator before any fix, all confirmed on ripgrep 15.2.0: `--smart-case` resolves across the whole `-e` set (`-e foo -e bar` matches, `-e foo -e Bar` matches nothing) — `smart` dropped from the enum; `QRegularExpression::escape` on a non-ASCII anchor exits 2 with a regex parse error — escaping removed in favour of `--fixed-strings`; a missing scope path exits 2 — pruning added (INV-11); six identical runs gave six different event orders under `--threads 4` — cap moved after the sort and every array given a stated order. A fifth correction came from the same runs: `--json` submatches carry each occurrence's matched text and offset, so attribution reads them rather than re-testing the line, which also settled `count` as occurrences (INV-1). Three test clauses were strengthened after being shown to pass vacuously (INV-7 ordering, INV-8 `files_count`, INV-3 accented anchor). § 4's "peak memory bounded by `max_cells`" was withdrawn — it contradicted the tally-before-cap the fix requires. Loop 2 dispatched. |
