# ANTS-3448 — marker-aware v2 un-triaged delta in `feedback_query` + `session_orient feedback_pending`

**Status:** Implemented (2026-07-05) — cold-eyes clean (3 loops; loop 3 = 0C/0H,
polish-converged). Marker-aware `FeedbackFile::parse()` (shared `markerVersion` +
hoisted `closureRe`/`bulletArmRe`) + `feedback_query` `format_version` /
`suspected_untagged[]` + `tests/features/feedback_query_v2_delta/` + the three
standard amendments (goes live on next relaunch).
**Kind:** enhancement
**Source:** in-session-2026-07-05 (v2 feedback-file redesign; the read-side change
that completes the v2 chain — migrate_v2/assign_id/compact_resolved are the write
side, this is the reader)

## 1. Problem

The v2 feedback-file format records triage **inline** (each `### ` finding carries
a `**Proposed ID:**` line) with no tracking table. The standard already specifies
the read contract — docs/standards/mcp-feedback-files.md §"The un-triaged delta"
gives the **v2 rule** (un-triaged = findings whose `**Proposed ID:**` is unfilled)
and §"Version selection" (v2 rule iff the file carries `<!-- ants-mcp-feedback:
2 -->`, else the v1 rule). But **the readers don't implement it yet.**
`FeedbackFile::parse()` computes only the **v1** delta (everything after the last
maintainer tracking table), and both consumers call it **unconditionally**,
ignoring the marker:

- `feedback_query` (`cmdFeedbackQuery`, remotecontrol.cpp) emits `pr.delta` /
  `delta_present` / `delta_line_count` / `delta_start_line` / `mapped_ids`.
- `session_orient`'s `feedback_pending` block (remotecontrol.cpp) reads
  `pr.deltaPresent` / `pr.deltaLineCount` for the per-file un-triaged count.

So on a migrated `: 2` file the readers still apply the **v1** rule: the delta is
keyed off the (retained, in-place) tracking-table watermark, not the inline
`**Proposed ID:**` state — a maintainer who fills a finding's slot via `assign_id`
sees **no** change in the un-triaged list. This spec makes `parse()` **marker-
aware** so a `: 2` file reads under the v2 rule.

This closes the loop that `migrate_v2` (ANTS-3446) deliberately left open: that
spec kept the tables in place precisely **because** the reader was marker-blind
(moving the watermark would have blanked the delta); with the reader taught the
v2 rule, an assigned id now correctly removes its finding from the delta.

## 2. Surface

All changes are in `ants_core_lib` (`FeedbackFile::parse`) plus the
`feedback_query` envelope. **`session_orient feedback_pending` needs no code
change** — it already consumes `deltaPresent` / `deltaLineCount`, which `parse()`
will now compute per version.

### 2.1 `parse()` becomes marker-aware

`parse()` gains a **format-version detection** and a **v2 delta branch**. The v1
computations that other callers depend on are **unchanged**:

- The `#`/`## ` boundary scan → `lastMaintainerLine`, `maintainerBlockCount`,
  `trackingRows` runs **on both versions**, verbatim. The compaction ops
  (`compactShipped` / `pruneTracking` for v1 tables; `migrateV2` for the
  watermark) read only these, so a migrated `: 2` file still exposes its retained
  v1 tables to them exactly as before (INV-6).
- Only the **delta fields** (`delta`, `deltaPresent`, `deltaStartLine`,
  `deltaLineCount`) and `mappedIds` are computed **per version**, and two new
  fields (`formatVersion`, `suspectedUntagged`) are added. (`parse()` currently
  builds `mappedIds` and `trackingRows` in **one interleaved Pass-2 loop**; the
  v2 branch keeps that loop for `trackingRows` and simply **overrides**
  `mappedIds` with the inline set afterward — the loop is not duplicated.)

**Version detection.** A new shared helper `FeedbackFile::markerVersion(content)`
returns the **version int** parsed from the **first**
`<!-- ants-mcp-feedback: N -->` line (the digit-optional
`<!--\s*ants-mcp-feedback:\s*([0-9]*)\s*-->` regex `migrate_v2` already uses,
`feedbackfile.cpp`), or `0` when the marker is absent or malformed. `parse()`
calls it and stores the result in `formatVersion`; `migrate_v2`'s version test is
refactored to call the same helper so the read and write sides extract the
version identically (INV-12). **`migrate_v2` keeps its own marker
*presence* detection*** — the helper supplies only the version int, and
`migrate_v2` still needs presence (insert a marker at line 1 when absent vs
replace in place when malformed) and re-locates the marker line itself; the helper
deliberately does **not** subsume that. `formatVersion >= 2` selects the
**v2 rule**; anything else (`0` / `1`) selects the **v1 rule**. (`>= 2`, not
`== 2`, so a future `: 3` reads under v2 rather than silently falling back to v1 —
the reader degrades forward, not backward; this matches `migrate_v2`'s own `>= 2`
idempotency short-circuit, ANTS-3446 § 2.7.) **Degrade-forward is a *reader*
policy** — the two readers (`parse` and `migrate_v2`'s no-op check) use `>= 2`; the
*compactor* `compact_resolved` deliberately keeps an exact `== 2` gate (a
compactor must not mutate an unrecognised future format), so a `: 3` file reads as
v2 but is refused `not_v2` by `compact_resolved` by design (§7).

### 2.2 The v2 delta rule (formatVersion >= 2)

Per the standard §"The un-triaged delta" v2 rule, computed over the shared
fence-aware `enumerateFindingBlocks` (ANTS-3443):

1. **Findings.** A finding is a `### ` block **with** a `**Proposed ID:**` line
   (`idLine0 >= 0`). A `### ` block **without** one is non-finding prose and is
   never in the delta.
2. **Un-triaged.** A finding is un-triaged iff its first `**Proposed ID:**`
   value (`idValue`, the enumerator's trimmed, `*`-stripped capture) holds **no**
   `ANTS-[0-9]+` token **and** does not begin `n/a` (case-insensitive `n/a\b`).
   This covers the empty value, the `_(maintainer to assign)_` placeholder, and
   **any** other non-id, non-`n/a` free text. A finding is **triaged** iff the
   value holds an `ANTS-[0-9]+` id **or** begins `n/a` (a closure). To share the
   closure test with `compactResolved` (whose `closureRe` is a function-local
   `static` today), **hoist that `^n/a\b` regex to an anonymous-namespace helper**
   (like the existing `fenceRe` / `maintainerAnchorRe`) that both `compactResolved`
   and `parse()` call, so the closure classification cannot drift (§7). This
   free-text classification follows the standard's §"Maintainer triage" value rule
   (any non-id, non-`n/a` value is un-triaged), which is broader than — and the
   authority over — the standard's §"The un-triaged delta" step-2 gloss ("empty or
   the placeholder"); §7 flags that gloss for reconciliation.
3. **Delta.** The delta is the ordered concatenation (document order, joined by
   `\n`) of every **un-triaged** finding's block text — each block is
   `lines[headingLine0 .. extentEnd0)` with **all** trailing blank lines stripped
   from that block (so the blank padding before the next `### ` is never counted,
   however many pad lines there are — this is a per-block strip, distinct from the
   v1 path's single whole-delta trailing-segment trim). This reuses
   the existing string-shaped `delta` field, so `feedback_query`'s `delta` output
   and `feedback_pending`'s `delta_line_count` stay meaningful without a new
   output shape:
   - `deltaPresent` = at least one un-triaged finding exists.
   - `deltaStartLine` = the **first** un-triaged finding's 1-based heading line.
   - `deltaLineCount` = the number of lines in the concatenated delta text.

   **Caveat (INV-11):** under v2 the delta is a *concatenation of (possibly
   non-contiguous) findings*, so — unlike the v1 delta — it is **not guaranteed**
   to equal the file slice `file[deltaStartLine .. deltaStartLine +
   deltaLineCount]` (it happens to coincide when exactly one finding is
   un-triaged, but not when two non-adjacent findings are). `deltaStartLine` is
   informational (where the first un-triaged finding sits), not a slice anchor; a
   consumer MUST treat `delta` as opaque text and MUST NOT re-derive it from
   `deltaStartLine`.
4. **Assigned-id set** (`mappedIds`). Under v2 this is the sorted, unique union of
   `ANTS-[0-9]+` from each finding's **first `**Proposed ID:**` line** (`idValue`),
   **contributed only when that line is not an `n/a` closure** (a closure
   contributes zero ids). The enumerator is first-id-line-wins, so this is exactly
   the one `idValue` per finding — **not** a scan past an `n/a` first line to a
   later id line, and **not** a whole-body scan (a finding may cite other ids in
   its prose) and **not** the v1 maintainer-block scan. On a migrated file the
   retained v1 tables' ids are **not** counted — under v2 the inline lines are the
   source of truth; the tables are read-only history.
5. **`suspectedUntagged`** (silent-loss guard). A `### ` block with **no**
   `**Proposed ID:**` line (`idLine0 < 0`) **but** carrying a finding bullet
   `^ *[-*] +\*\*(what|repro|impact)[:*]` (case-insensitive, fences skipped —
   the same **bullet arm** the standard's guard uses) is a real finding a hand
   editor forgot to tag. It is **not** in the delta (no id line) but is recorded
   in `suspectedUntagged` so `feedback_query` can surface it rather than let it
   vanish as prose. A bare-prose `### ` block (no id line, no finding bullet) is
   ordinary prose and is **not** recorded. (This bullet-only test is narrower
   than `migrate_v2`'s finding-shaped discriminator, which also has a heading
   arm; the standard scopes `suspected_untagged` to the bullet arm — ANTS-3446
   § 2.4 — so the two intentionally differ and the reader matches the standard.)
   The bullet regex is `migrate_v2`'s `bulletArmRe`, which is a function-local
   `static` today; **hoist it to a shared anonymous-namespace helper** used by
   both `migrateV2` and `parse()` so the two bullet tests are one definition (§7).

The v1 rule (`formatVersion < 2`) is retained **verbatim** — the current
boundary-watermark delta, `mappedIds` from maintainer-block bodies, no
`suspectedUntagged` (left empty).

### 2.3 `feedback_query` envelope additions

`cmdFeedbackQuery` gains two output fields (both present on every **success**
reply, so a consumer can branch without a second call):

- `format_version` — the `int` `parse()` detected (0 / 1 / 2 / …). Lets a caller
  see which rule produced the delta.
- `suspected_untagged` — an array of `{heading, line}` (1-based heading line) for
  every §2.2-step-5 block. **Empty on a v1 file** (the v1 rule leaves it empty),
  and empty on a clean v2 file. All existing fields keep their names and
  semantics — the always-present `ok` / `path` / `delta` / `delta_present` /
  `delta_line_count` / `delta_start_line` / `mapped_ids` / `truncated` /
  `maintainer_block_count` / `last_maintainer_line`, and the conditional
  `path_derived` (only when the path was derived) / `tracking` (only under
  `include_tracking`) — with the two new fields purely **additive**. On a `: 2`
  file `delta` / `delta_*` / `mapped_ids` now follow the v2 rule, while
  `maintainer_block_count` / `last_maintainer_line` / `tracking` still reflect the
  retained v1 tables (unchanged).

The `feedback_query` tool **description** (`claudeintegration.cpp`) enumerates the
output fields and today opens with the v1-only framing *"everything a contributor
appended after the last maintainer tracking block"* and *"`mapped_ids` are the
ANTS-NNNN ids already cited in maintainer blocks"* — both must be updated on ship
to name the version-dependent rule and add `format_version` + `suspected_untagged`
to the enumerated list (§7).

The byte-cap / truncation logic on `delta` (ANTS-1855) is unchanged — it clips
the concatenated v2 delta text exactly as it clips the v1 delta text.

### 2.4 `session_orient feedback_pending` — no code change

The block reads `pr.deltaPresent` (skip fully-triaged files) and
`pr.deltaLineCount` (the per-file pending count + `total_pending_lines`). Both are
now version-correct straight from `parse()`, so a `: 2` file's pending count is
its unfilled-`Proposed ID` finding lines, not the v1 after-table region — the
ANTS-1964 `feedback_pending` block gets the v2 rule **for free**. (This is the
whole reason the delta is expressed through the same `deltaPresent` /
`deltaLineCount` fields rather than a new list output — §2.2 step 3.)

### 2.5 Worked example

A migrated `: 2` file, one filled finding + one still-blank + one prose note +
one un-tagged finding-shaped block, with a **contributor session heading**
(`## …`) after the table so the v1/v2 contrast is real:

```markdown
<!-- ants-mcp-feedback: 2 -->
# Feedback TEST

## 📋 Ants Terminal roadmap tracking (2026-06-01, maintainer)

| Item | ID | Status |
|---|---|---|
| timeout | ANTS-1400 | ✅ |

## 2026-06-20 — Vestige session

### Issue #1 — filled
- **Proposed ID:** ANTS-1525
- **What:** done.

### Issue #2 — still blank
- **Proposed ID:** _(maintainer to assign)_
- **What:** open.

### Positive note — nice
It felt fast.

### Issue #3 — forgot to tag
- **What:** a real gap, no id line.
```

Under the v2 rule: `Issue #1` is triaged (has an id) → not in the delta;
`Issue #2` is un-triaged (placeholder) → **the delta** (`delta_present:true`,
`delta_start_line` = its heading line, `delta_line_count` = its block lines);
`Positive note` is prose (no id line, no finding bullet) → ignored; `Issue #3`
has finding bullets but no id line → **`suspected_untagged`**, not the delta.
`mapped_ids` = `["ANTS-1525"]` — the retained table's **`ANTS-1400`** is **not**
counted (under v2 the inline `**Proposed ID:**` lines are the source of truth;
the table is read-only history). `maintainer_block_count:1` /
`last_maintainer_line` still point at the retained table.

Under the **old** (v1, marker-blind) reading the delta would instead start at the
`## 2026-06-20 — Vestige session` boundary and run to EOF — engulfing **all four**
`### ` blocks (triaged, prose, and un-tagged alike), because the v1 rule keys off
the `#`/`## ` boundary after the watermark, not the inline id state. (Note: the
`## ` session heading is load-bearing to this contrast — without a `#`/`## `
heading after the table the v1 delta would be *empty*, since the v1 scanner
treats `### ` as inert; a real migrated corpus file always wraps post-table
findings under such a session heading.) That over-broad v1 delta is exactly what
the marker-aware fix corrects.

## 3. Invariants

- **INV-1** — `parse()` detects `formatVersion` from the first
  `<!-- ants-mcp-feedback: N -->` line (absent/malformed → 0); `>= 2` selects the
  v2 delta rule, `< 2` the v1 rule (source-grep + unit test on both).
- **INV-2** — v1 unchanged: on a `: 1` / marker-absent / malformed-marker file,
  `delta`, `deltaPresent`, `deltaStartLine`, `deltaLineCount`, `mappedIds` are
  **byte-identical** to the pre-change output, and `suspectedUntagged` is empty.
  (Regression-locked by feeding a v1 fixture and asserting the exact prior delta.)
- **INV-3** — v2 delta = ordered concatenation of un-triaged findings (a `### `
  block with a `**Proposed ID:**` line whose value holds no `ANTS-[0-9]+` and
  does not begin `n/a`). A finding with an id or an `n/a` closure is **not** in
  the delta; a `### ` block with no id line is **not** in the delta.
- **INV-4** — v2 `mappedIds` = the sorted-unique union of `ANTS-[0-9]+` from each
  finding's **first** `**Proposed ID:**` value (`idValue`), contributed only when
  that value is **not** an `n/a` closure; the retained v1 tables' ids are not
  counted.
- **INV-5** — `suspectedUntagged` (v2 only) lists every `### ` block with no id
  line but a `- **What/Repro/Impact:**` bullet (fences skipped); a bare-prose
  block is excluded; on v1 it is empty.
- **INV-6** — the v1 boundary scan is version-independent:
  `lastMaintainerLine`, `maintainerBlockCount`, `trackingRows` are computed the
  same on `: 1` and `: 2`, so `compactShipped` / `pruneTracking` / `migrateV2`
  (which read only these) are unaffected by the version branch. (Grep: those
  helpers never read `delta*` / `mappedIds` / `suspectedUntagged`.)
- **INV-7** — fenced regions are inert in both the finding enumeration
  (`enumerateFindingBlocks`) and the finding-bullet scan (fence-aware), so a
  `### `/`**Proposed ID:**` line inside a ``` fence never perturbs the v2 delta.
- **INV-8** — `feedback_query` emits `format_version` (the detected int) and
  `suspected_untagged[]` (`{heading, line}`) on every **success** reply (an early
  refusal envelope — `not_found` / `read_failed` / `not_feedback_file` — carries
  neither); both are additive (no existing field renamed or dropped). The `delta`
  byte-cap/truncation is applied to the v2 delta identically.
- **INV-9** — `session_orient feedback_pending` needs **no** code change; its
  `delta_line_count` / `files_with_pending` counts follow the v2 rule via
  `parse()` on a `: 2` file (verified by driving `cmdSessionOrient` — or the
  parse path it uses — over a `: 2` fixture).
- **INV-10** — idempotent read: `parse()` is pure and read-only; no field of the
  file is written by either consumer on the query path.
- **INV-11** — under v2 the `delta` is a concatenation of findings, so it is
  **not guaranteed** to equal the file slice `file[deltaStartLine ..
  deltaStartLine + deltaLineCount]` (it coincides for a single un-triaged finding
  but not for two non-adjacent ones); `deltaStartLine` is informational (the first
  un-triaged finding's heading line), not a slice anchor, and a consumer MUST NOT
  re-derive `delta` from it. (Under v1 the three fields remain a contiguous slice,
  as the `feedbackfile.h` `ParseResult` comments state — those comments are
  updated to note the v2 exception, §7.)
- **INV-12** — `formatVersion` detection and `migrate_v2`'s version test both go
  through the shared `FeedbackFile::markerVersion` helper (§2.1), so the read and
  write sides extract the version identically (a file `migrate_v2` marks `: 2`
  always reads back as v2). The helper returns the version int only; `migrate_v2`
  retains its own marker presence + line-index detection for its insert-vs-repair
  branch (the helper does not subsume it).

## 4. RAM / build cost

The v2 branch adds one `enumerateFindingBlocks` pass (O(lines)) + a per-finding
id-line classification and a per-candidate finding-bullet scan — only on a `: 2`
file; a v1 file keeps its existing single boundary scan. No new allocation class
(the parse is transient), no new build target, no new dependency. The
`suspectedUntagged` vector is bounded by the `### ` block count (already
enumerated). `feedback_query`'s two added fields are O(findings) JSON. (Because
`compactShipped` / `pruneTracking` have no version gate, their internal `parse()`
on a migrated `: 2` file also pays the v2 branch's one extra O(lines) pass; the
result is discarded by those ops — a negligible, harmless cost.)

## 5. Out of scope

- **Rendering an on-demand roadmap-status view** (`feedback_query` resolving each
  assigned id's live `ROADMAP.md` status) — the standard's §"Tooling" mentions it
  as a v2 `feedback_query` capability, but it is a **separate** change (it needs a
  roadmap read, which the delta rule does not); this spec is the delta/marker
  routing only.
- **Deprecating the v1 rule** — it stays, verbatim, for un-migrated files
  (the whole corpus is still `: 1`).
- **Changing `session_orient feedback_pending`'s shape** — it keeps its
  per-file `{file, delta_line_count}` entries; only the underlying count rule
  changes (via `parse()`).
- **A structured `untriaged_findings[]` output** on `feedback_query` (a list of
  finding headings instead of the concatenated `delta` text) — deferred; the
  string `delta` is retained to keep the two consumers unchanged in shape (§2.2
  step 3). Add it only if a consumer needs per-finding structure.

## 6. Tests

Feature-conformance `tests/features/feedback_query_v2_delta/`: `spec.md` +
`test_feedback_query_v2_delta.cpp` (pure `FeedbackFile::parse` over synthetic v1
and v2 fixtures + a live `cmdFeedbackQuery` drive + a `feedback_pending`
count check + schema/source greps). Cases cover INV-1…INV-12:

- **v1 regression** (INV-2): a v1 fixture (tracking table + a `## ` session
  heading + post-table findings) → `delta`/`deltaStartLine`/`deltaLineCount`/
  `mappedIds` byte-identical to the current output; `suspectedUntagged` empty;
  `formatVersion` = 1.
- **v2 delta** (§2.5 / INV-3): the §2.5 fixture → the delta is exactly
  `Issue #2`'s block; `Issue #1` (id) and `Issue #3` (no id line) excluded;
  `delta_start_line` = `Issue #2`'s heading line.
- **v2 concatenation of ≥2 non-contiguous findings** (INV-3/11): a fixture
  ordered (un-triaged, triaged, un-triaged) → `delta` = the two un-triaged blocks
  joined by `\n` (trailing blanks trimmed per block), `deltaLineCount` = their
  summed line counts, `deltaStartLine` = the first un-triaged finding's heading
  line — and `delta` is verified **not** equal to the file slice from
  `deltaStartLine` (the non-slice caveat, INV-11).
- **v2 triaged excluded** (INV-3): a finding with `- **Proposed ID:** ANTS-1525`
  and one with `- **Proposed ID:** n/a — folded` are both out of the delta.
- **v2 mapped ids** (INV-4): `mapped_ids` = the inline assigned ids only —
  including a two-id union from one finding (`- **Proposed ID:** ANTS-1525,
  ANTS-1579` → both in `mapped_ids`); a retained table id not on any finding line
  is **not** included; and a closure that names an id
  (`- **Proposed ID:** n/a — folded into ANTS-1525`) contributes **zero** ids —
  `ANTS-1525` is **absent** from `mapped_ids`.
- **suspected_untagged** (INV-5): the `Issue #3` no-id finding-shaped block is
  listed `{heading, line}`; a `### Positive note` prose block is not.
- **version detection** (INV-1): `: 2` → v2 rule; `: 1`, malformed
  `<!-- ants-mcp-feedback: -->`, and marker-absent → v1 rule; a future `: 3` →
  v2 rule (`>= 2`).
- **compaction-op non-regression** (INV-6): `parse(: 2 fixture).lastMaintainerLine`
  / `maintainerBlockCount` / `trackingRows` equal what a v1-shaped read of the
  same tables yields (the version branch doesn't disturb them).
- **fence safety** (INV-7): a `### `/`**Proposed ID:**` line inside a ``` fence is
  neither a delta finding nor a `suspected_untagged` entry.
- **feedback_query envelope** (INV-8): `format_version` + `suspected_untagged[]`
  present; the delta byte-cap truncates a large v2 delta and sets `truncated`.
- **feedback_pending count** (INV-9): a `: 2` fixture with N unfilled findings →
  the `feedback_pending` per-file `delta_line_count` reflects the v2 rule (not the
  v1 after-table count).
- **migrate_v2 → parse round-trip** (INV-12): feed a v1 fixture through
  `migrateV2`, then `parse()` its output → `formatVersion` = 2 and the un-triaged
  delta = the stamped-placeholder findings (the write and read sides agree on the
  version via the shared `markerVersion`).
- **read purity** (INV-10): `parse()` over any fixture leaves the input string
  unchanged (it takes `const QString&`); neither consumer writes the file on the
  query path.

## 7. Cross-doc impact

- `feedbackfile.h` / `feedbackfile.cpp` — `ParseResult` gains `int
  formatVersion = 0;` and `QVector<SuspectedFinding> suspectedUntagged;` (a new
  `struct SuspectedFinding { QString heading; int line = -1; };`). A new shared
  helper `FeedbackFile::markerVersion(const QString &content)` returns the parsed
  version (0 when absent/malformed) using the digit-optional marker regex
  `migrate_v2` already uses; **`migrateV2`'s version test calls it** while keeping
  its own marker presence + line-index scan (INV-12). To make the "shared, never
  drifts" claims real, **hoist two currently function-local `static` regexes to
  anonymous-namespace helpers** (alongside `fenceRe` / `maintainerAnchorRe`):
  `compactResolved`'s closure regex (`^n/a\b`) and `migrateV2`'s `bulletArmRe`
  (`^ *[-*] +\*\*(what|repro|impact)[:*]`), so `parse()`'s v2 branch and the
  original owners share one definition each. `parse()` calls `markerVersion` + adds
  the `formatVersion >= 2` delta branch (reusing `enumerateFindingBlocks` +
  `fenceOpenerChar` + the two hoisted regexes — no new scanner). The
  **`ParseResult` field comments** for `delta` / `deltaStartLine` /
  `deltaLineCount` are updated to note they are a contiguous slice only under v1;
  under v2 the delta is a concatenation and `deltaStartLine` is informational
  (INV-11). The v1 path is otherwise unchanged. (Note: `compact_resolved`'s
  version gate is left as-is — it uses a **digit-required** `([0-9]+)` regex and
  an **exact `== 2`** test on purpose (ANTS-3443, `remotecontrol.cpp`), NOT the
  reader's `>= 2`: a *compactor* must refuse to act on an unrecognised future
  format (a `: 3` file it doesn't understand), whereas the *reader* degrades
  forward. It stays exact and does not adopt `markerVersion` because it is
  shipped + tested and wants that stricter gate — see §2.1.)
- `remotecontrol.cpp` — `cmdFeedbackQuery` emits `format_version` +
  `suspected_untagged[]`. `session_orient feedback_pending` is **untouched**
  (INV-9).
- `claudeintegration.cpp` — the `feedback_query` tool **description** enumerates
  its output fields, so it **must** be updated (not "if"): add `format_version` +
  `suspected_untagged` to the `Returns {…}` list, and reword the v1-only summary
  (*"everything a contributor appended after the last maintainer tracking block"*
  → the version-dependent rule; *"`mapped_ids` are the ANTS-NNNN ids already cited
  in maintainer blocks"* → "…the assigned inline `**Proposed ID:**` ids on a v2
  file, or maintainer-block ids on v1"). No input-schema change.
- `docs/standards/mcp-feedback-files.md` — the v2 delta rule + version selection
  are **already written** (§"The un-triaged delta", §"Version selection"); on
  ship, (a) flip the §"Format version 2" / §"Tooling" **impl-status** notes that
  say the v2 read path is "still not built" / "keeps the v1 path" to record that
  `feedback_query` + `feedback_pending` now honour the marker (ANTS-3448), **and**
  (b) amend §"Version selection" from "v2 rule **iff** it carries `: 2`" to the
  degrade-forward `>= 2` (a `: 3` file reads under v2), matching this spec's INV-1
  and `migrate_v2`'s `>= 2` short-circuit — a small contract refinement, not a
  no-op; **and** (c) reconcile the §"The un-triaged delta" **step-2 gloss**
  ("un-triaged iff value is empty or the `_(maintainer to assign)_` placeholder")
  with the broader, authoritative §"Maintainer triage" value rule (any non-id,
  non-`n/a` free-text value is un-triaged) that this spec's §2.2 step 2 follows —
  the gloss is narrower than the parser it describes.
- `docs/standards/mcp-error-codes.md` — no new refusal code (the query path adds
  no refusal; a malformed marker degrades to the v1 rule, not an error).
- `op:migrate_v2` (ANTS-3446) / `op:assign_id` (ANTS-3447) / `op:compact_resolved`
  (ANTS-3443) — the write side; this reader is what makes an `assign_id` fill
  visibly shrink the delta. Together they are the full v2 lifecycle.
- Parent roadmap bullet **ANTS-3448** — this spec; flips ✅ when the reader ships.

## Cold-eyes loop log

Two reviewers per loop, cold (loop N+1 briefed identically to loop 1 — never
told what was fixed).

- **Loop 1** — 0C, 1H, 6M, ~7L (deduped across both lanes). Both lanes verified
  the load-bearing structural claims **sound**: INV-6 (the compaction ops read
  only `lastMaintainerLine`/`trackingRows`, never `delta*`/`mappedIds`), §2.4
  (`feedback_pending` reads only `deltaPresent`/`deltaLineCount` → no code
  change), and `mappedIds` is consumed only by `cmdFeedbackQuery`. **H:** the
  §2.5 worked example's v1 contrast was false — its post-table headings were all
  `### ` (inert to the v1 `#`/`## ` scanner), so v1 would yield an *empty* delta,
  not "all four blocks"; reworked the fixture with a `## ` session heading + a
  real `ANTS-1400` table id so both the v1-over-broad contrast and the
  "retained-table id excluded" point actually hold. **M:** (a) `>= 2` vs the
  standard's "iff `: 2`" — reconciled by amending the standard's Version-selection
  to degrade-forward `>= 2` (§7); (b) `mappedIds` "first non-`n/a` line" wording
  contradicted the first-id-line-wins scanner — restated as "first id line,
  contributed only when not an `n/a` closure"; (c) the v2 `delta` is a
  concatenation of non-contiguous findings, so `deltaStartLine` is **not** a slice
  anchor — added INV-11 + a `feedbackfile.h` comment update + a ≥2-finding test;
  (d) trailing-blank handling per v2 block was undefined (→ trim, matching v1);
  (e) `claudeintegration.cpp` description **does** enumerate outputs (so the
  update is mandatory + the stale v1 summary needs rewording) — made §7 definite;
  (f) marker detection "its own regex" invited read/write drift → mandated a
  shared `markerVersion` helper (INV-12). **L:** step-2 lead-in undersold the
  rule (any non-id/non-`n/a` text is un-triaged); reuse `compactResolved`'s
  closure regex; existing-fields list add `ok`/`path`/`path_derived`; shared
  Pass-2 loop framing. All verified fixes applied.

- **Loop 2** — **0C, 0H** (both lanes); every loop-1 fix re-verified sound
  (feedback_pending no-change, INV-6, the reworked §2.5 example, mappedIds
  first-line-wins, fence-inertness, all field names — confirmed against code).
  Residual was **mechanical precision on the loop-1 fixes themselves**: (M1, both
  lanes) the `markerVersion` helper as specified (int only, absent≡malformed≡0)
  couldn't drive `migrate_v2`'s insert-vs-repair branch — scoped the refactor so
  `migrate_v2` keeps its own presence + line-index scan and shares only the
  version extraction (INV-12); (M2, both) the "reuse `compactResolved`'s closure
  regex" / "`migrate_v2`'s `bulletArmRe`" claims were infeasible (function-local
  statics) — prescribed **hoisting both to anon-namespace helpers** (§7); (M3,
  both) the per-block trailing-blank trim was mis-analogised to the v1 single-trim
  and left one-vs-all ambiguous — pinned to "strip **all** trailing blanks per
  block"; (M2 lane B) INV-11 over-claimed (a single un-triaged finding **is** a
  contiguous slice) — softened to "not guaranteed"; (M4/L2) the classification
  cite moved to §"Maintainer triage" (the authoritative value rule) with a
  standard step-2-gloss reconciliation added to §7; conditional-field wording
  (`path_derived`/`tracking`); a closure-with-embedded-id `mapped_ids` oracle
  added to §6. All verified fixes applied.

- **Loop 3** — **0C, 0H** (both lanes); every loop-2 fix re-verified against code
  (`closureRe`/`bulletArmRe` confirmed function-local statics; the v1 single-trim;
  the interleaved Pass-2 loop; the reworked §2.5 example; `feedback_pending`
  no-change; all field names byte-exact). Both lanes surfaced the **same** single
  MEDIUM: `compact_resolved`'s gate is exact `== 2`, which diverges from the
  reader's `>= 2` degrade-forward (a `: 3` file reads as v2 but is refused
  `not_v2` by the compactor) — a **cross-verb rationale** point, not a defect in
  the reader; documented as a deliberate exception (a compactor must not mutate an
  unrecognised future format) in §2.1 + §7. Plus LOW/precision: "marker line
  index" → "re-locates the marker line"; INV-8/§2.3 "every reply" → "every
  **success** reply"; a two-id-union `mapped_ids` oracle, an INV-12
  migrate→parse round-trip case, and an INV-10 purity case added to §6; a §4
  clause on the compaction ops paying the v2 pass. All applied.

- **Convergence (2026-07-05):** 3 loops. Loops 2 and 3 both returned 0C/0H;
  the loop-3 residual was one cross-verb documentation point + wording precision
  → **polish-converged** (no structural/mechanical/architectural finding). Every
  load-bearing claim was re-verified live across the loops: INV-6 (compaction ops
  read only `lastMaintainerLine`/`trackingRows`), §2.4/INV-9 (`feedback_pending`
  needs no code change), the v2 rule matches the standard's §"Maintainer triage"
  value rule, `mappedIds` first-id-line-wins + closure-contributes-zero, fence
  inertness, the reworked §2.5 example's v1/v2 contrast, and the
  function-local-static regex hoists. Signed off as implementation-ready; the
  implementer's cross-doc commitments are the §7 items (the `markerVersion` +
  regex hoists, the `ParseResult` comment update, the `claudeintegration.cpp`
  description update, and the three standard amendments).
