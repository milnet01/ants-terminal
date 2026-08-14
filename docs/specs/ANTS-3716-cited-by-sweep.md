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
  `workspace_search` already owns the regex case. Each is `QRegularExpression::
  escape`d before it reaches rg. Empty array or > 64 entries refuses `bad_args`.
- **`scope`** (optional, project-relative files and/or dirs) — defaults to the
  three targets `/apply-fixes` § 4 names: the project's `docs_dir` (via
  `ProjectSettings::load(rootCanonical).docsDir`, else `docs`), `README.md` and
  `CLAUDE.md`. Every entry runs through `PathValidation::validatePath`; a
  root-escaping entry refuses `bad_path`.
- **`case`** (optional) — `insensitive` (default), `sensitive`, `smart`.
- **`max_cells`** (optional, default 500, clamp 1–5000).

### 2.2 Why the case default differs from `workspace_search`

`workspace_search` defaults to `smart`. This verb defaults to **`insensitive`**,
deliberately, and the reason is the purpose rather than consistency: a cell the
sweep never produced is a document nobody opens, which is the documented failure
mode above, whereas a cell that turns out to be a coincidental match costs one
`agrees` verdict. The two errors are not symmetric here.

`/write-code`'s own trigger table records the measurement that settles it — of a
changed UI string, *"a case-sensitive grep … missed the test asserting it
`.lower()`-ed"*. The divergence is stated in the verb description so a caller
reading the two side by side does not read it as an inconsistency.

### 2.3 One pass, with attribution

The whole point is one traversal of the doc set that still attributes each hit
to its anchor. rg takes repeated `-e` patterns, so:

1. Build one argv with `-e <escaped anchor>` per anchor, plus `--json`, the
   `case` flag, and the resolved scope paths.
2. Run it once. rg's `--json` match events carry the matched **line**, not which
   `-e` produced it.
3. Attribute each matched line by testing every anchor against it, under the
   same case rule. Exact for literals, which is why `anchors` excludes regexes —
   with a regex there is no substring test that reproduces rg's own match.

A line citing two anchors yields **two cells**, which is correct: it needs a
verdict against each.

rg is invoked exactly as `cmdWorkspaceSearch` does today —
`QProcess::start(QString, QStringList)` with the argv list, no shell
(`src/remotecontrol_workspace.cpp:309`–`:314`, the ANTS-1248 INV-3 contract).
That call site is currently inline inside `cmdWorkspaceSearch`, an ~800-line
function; this spec **extracts it** to a file-local helper both verbs call,
rather than copying it, so the no-shell contract keeps one owner.

### 2.4 Response

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

- `cells` sorted by (anchor, file) so two runs over an unchanged tree are
  byte-identical.
- **`anchors_unmatched` is a first-class result, not an omission.** "Nothing
  cites this" is the cheap half of the sweep and the caller needs it stated;
  inferring it from an absence is how a caller silently drops an anchor it
  mistyped.
- No line bodies. The caller opens the file to record a verdict regardless, and
  `first_line` is enough to get there. `read_region` is the drill-in.

ETag-supported (`isEtagSupportedTool`) like the other read verbs.

## 3. Invariants

- **INV-1** — Every (anchor, file) pair with at least one match appears as
  exactly one cell, carrying that pair's total match count. Two anchors on one
  line produce two cells. *Test:* `tests/features/cited_by/` — a fixture doc
  citing two anchors on one line and one anchor twice on separate lines → 3
  cells, counts 1, 1, 2.
- **INV-2** — An anchor with no match anywhere in scope appears in
  `anchors_unmatched` and in no cell; an anchor with a match appears in
  `anchors_matched` and never in `anchors_unmatched`. The two arrays partition
  `anchors` exactly. *Test:* `tests/features/cited_by/` — fixture with one cited
  and one uncited anchor → the arrays partition, and their union equals the
  request's `anchors`.
- **INV-3** — An anchor is matched literally: regex metacharacters in an anchor
  match themselves. *Test:* `tests/features/cited_by/` — anchor `foo.cpp`
  against a doc containing `fooXcpp` and `foo.cpp` → one cell, `count` 1.
- **INV-4** — The default `case` is `insensitive`: an anchor cited in a
  different case still produces a cell. *Test:*
  `tests/features/cited_by/` — anchor `oldName` against a doc writing
  `oldname` → one cell with no `case` in the request; zero cells with
  `case:"sensitive"`.
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
- **INV-7** — `cells` is sorted by (anchor, file), so two runs over an unchanged
  tree return byte-identical bodies. *Test:*
  `tests/features/cited_by/` — run twice over one fixture, compare the
  serialized envelopes.
- **INV-8** — At `max_cells`, the response carries `truncated:true` and
  `cells_count` reports the **capped** array length while `files_count` is
  uncapped. A cap that reads as completeness is the defect this names. *Test:*
  `tests/features/cited_by/` — fixture producing 5 cells with `max_cells:2` →
  `cells.size()==2`, `truncated:true`.
- **INV-9** — The extracted rg helper starts the process with
  `QProcess::start(QString, QStringList)` and no shell, and both
  `cmdWorkspaceSearch` and `cmdCitedBy` route through it. *Test:* source scrape
  over `src/remotecontrol_workspace.cpp` asserting exactly one
  `rg.start(` call site in the file and that neither handler names `QProcess`
  directly.

## 4. RAM / build cost

No new library, no new build target, no on-disk state — the verb is a pure
function of (tree, request) and writes nothing, so it needs no cache-key or
relocation contract. Peak memory is bounded by `max_cells` × a cell (~120 bytes)
plus rg's `--json` output, which is streamed and discarded per event rather than
buffered whole. At the 5000-cell ceiling that is well under 1 MB. `anchors` is
capped at 64 so the argv cannot grow unbounded.

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

`spec.md` beside it maps INV-1…INV-9 to cases. Per the project convention each
case is verified to **fail against pre-fix code** before the implementation is
restored; INV-9's source scrape is the one case that cannot (it asserts a refit,
so it fails by construction until the extraction lands).

INV-6's two refusals and INV-8's cap are the cases most likely to pass
vacuously — a handler that refuses everything satisfies INV-6 — so each pairs
its refusal assertion with a positive control in the same test.

## 7. Cross-doc impact

- `docs/standards/mcp-tools.md` — no change; the verb follows the existing
  response-wrap, `caller_cwd`, path-validation and ETag contracts as written.
- `docs/standards/mcp-error-codes.md` — no new code; `bad_args` and `bad_path`
  are both existing.
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
