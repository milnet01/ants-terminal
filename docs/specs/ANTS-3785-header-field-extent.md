# ANTS-3785 — Spec header fields may wrap: one extent rule, one implementation

**Status:** shipped (2026-08-02, unreleased) — implemented test-first, both
writer tests verified RED against pre-fix code first; full suite 3194/3194.
Cold-eyes converged at the 3-loop cap (34 findings verified and fixed, none
deferred). This field wraps onto three lines on purpose: it is the shape the
rule exists to handle, and it is read and rewritten correctly now.
**Kind:** fix.
**Source:** ROADMAP.md ANTS-3785 (hit 2026-08-01 while shipping ANTS-3766) and
ANTS-3672 (found 2026-07-28 verifying ANTS-3665 against the live corpus).
**Covers:** ANTS-3785 (writer corrupts a wrapped field), ANTS-3672 (reader
truncates a wrapped field).
**Pairs with:** `docs/standards/specs.md` § 3.2 (header block) — the normative
home of the rule this spec implements — and [ANTS-1963](ANTS-1963.md), whose
INV-5 currently mandates the defective behaviour; § 7 carries the amendment.

## 1. Problem

A spec's header block is a run of `**Field:** value` lines under the H1
(`docs/standards/specs.md` § 3.2). The standard tells authors to grow the
Status field as work proceeds — *"Append cold-eyes progress inline as it
happens"* — and the corpus is hard-wrapped at ~80 columns, so Status routinely
runs onto continuation lines.

Nothing in the standard says a field **may** wrap or where one **ends**. Both
consumers independently assumed one physical line, and both are wrong in a
different direction:

| Site | Symbol | Defect | Roadmap id |
|---|---|---|---|
| Reader | `SpecParse::parseSpecBody` | `statusRe` is `^\*\*Status:\*\*\s*(.+?)\s*$` with `MultilineOption`, so `$` stops at the first physical line and every continuation is dropped | ANTS-3672 |
| Writer | `SpecLog::setStatus` | replaces only the matched line, orphaning the continuations as a stray paragraph — and returns `ok` | ANTS-3785 |

Neither failure is loud. The reader returns a status truncated mid-sentence;
the writer leaves a file that still parses, whose Status still resolves, and
whose orphaned fragment reads like a hand-edit somebody botched. `doc_integrity`
and `spec_lint` both pass it, because a stray paragraph is not a structural
defect.

**Scale.** 49 of the 172 specs carrying a Status field have a wrapped one —
every one of them read wrong today, and every one a corruption waiting for the
next `set_status`. `Kind` wraps once, of the 149 specs carrying one.

Every corpus figure this spec quotes is produced by
`tools/spec-header-survey.py` (shipped with this change; precedent
`tools/roadmap-corpus-survey.py`), so the numbers are an output rather than a
transcription and a reader can re-derive them after the corpus moves — which it
does continuously, this spec's own arrival having moved the denominator:

```sh
python3 tools/spec-header-survey.py docs/specs
# wrapped Status: 49 of 172
# wrapped Kind: 1 of 149
# first field inside a fence: 0
# fence opened inside a header block: 0
# orphaned-continuation signature: 0
# distinct header-field names: 46 (4 prose-shaped, each needing a human look)
# prose-bullet continuation (ANTS-1436.md): present
```

`--values` additionally prints each wrapped field's `line_count` and joined
value — the two outputs `headerField()` must reproduce, which is what makes the
tool a parity oracle rather than only a counter.

**Why one spec for two ids.** The two defects are one missing rule with two
faces. Fixing either alone means writing "where does a header field end" once
and then writing it again — the divergence `MarkdownScan` (ANTS-3603) and
`SpecParse` (ANTS-3665) were both hoisted to prevent. `specs.md` § 2 permits
one umbrella spec whose header lists the ids it covers; this is that.

## 2. Surface

### 2.1 The extent rule (normative)

**The search for a field is bounded to the header block** — from the top of the
document to the first `^##\s` heading, or EOF if there is none. Unbounded, a
scan for an absent field runs to the end of the file and can match a
`**Status:**` line quoted inside a fenced example, which would make the writer
rewrite inside a code fence and report success — the exact failure this spec
exists to end. The bound is what keeps INV-7's `unrecognised_format` reachable
instead of being accidentally suppressed by a fenced match further down.

Within that block, a field begins on a line matching
`^\*\*(?<name>[^*:]+):\*\*` and extends through every following line until the
first line that is **any** of:

- blank (`^\s*$`),
- another field marker (`^\*\*[^*:]+:\*\*`),
- an ATX heading (`^#{1,6}\s`) — headings may interrupt a paragraph, so a
  header block written without its blank separator must not swallow § 1.

Running out of lines ends it too, though end-of-file is a position rather than a
line. Every anchor is written against LF-split lines with no leading-whitespace
tolerance — this corpus is LF-only and its header blocks are unindented, and
admitting CommonMark's up-to-three-space heading indent here would buy nothing
while widening what can terminate a value. These are the **extent** terminators; the header-block bound above is a
separate, earlier constraint on *where a field is looked for at all*. They
coincide in practice — the `^##\s` that ends the block is also an ATX heading —
and they are stated apart because INV-1 tests the terminators and INV-10 tests
the bound.

**A line terminates the field if it matches ANY of those; it is a continuation
only if it matches NONE.** There is no precedence to resolve — the set is
unordered, and membership is the whole test.

The field's **value** is its own trailing text plus each continuation line,
each stripped of surrounding whitespace and joined with a single space.

**Only a marker at the START of a line opens or closes a field.** A bold
run ending in a colon *within* a line is ordinary value text — every anchor
above is `^`-anchored for this reason. Two corpus cases pin it:
`docs/specs/ANTS-3766-roadmap-migration-archives.md` carries
`**Split at loop 4:**` inside its Status prose, and `docs/specs/ANTS-1253.md`
packs `**Lanes:**` onto the same physical line as its `**Kind:**` value.

**The consequence has to be stated, because it is the rule's one sharp edge: a
value containing a bold colon-run is safe only while the line wrap keeps that
run off column 0.** `ANTS-3766`'s `**Split at loop 4:**` sits mid-line by where
its ~80-column wrap happened to land; one word earlier in the preceding clause
and it would begin a line, match the field-marker terminator, and truncate the
value — after which `setStatus` would orphan everything past it, reproducing
this spec's own defect on a header that breaks no stated rule.

No reader can resolve this, because a line-initial `**Foo:**` is
*indistinguishable* from a new field — that is what a marker looks like. The
field-name vocabulary is not closed either (`specs.md` § 3.2 enumerates ten and
the corpus also carries `Lanes`, `Siblings` and `Applies to`), so a
known-names-only terminator would silently swallow the next real field the day
someone adds an eleventh. It is therefore fixed on the **authoring** side, in
§ 2.3's amendment, and left detectable rather than repaired here.

`ANTS-1253` packs two fields onto one physical line. `specs.md` § 3.2 does not
currently forbid that — it says only "Bold key-value lines immediately under the
H1" — so § 2.3's amendment adds the one-field-per-line requirement that the
extent rule depends on. Until a header is rewritten to satisfy it, this rule
reads it faithfully rather than repairing it: `Kind` in `ANTS-1253` returns the
whole logical run, `**Lanes:**` included. That is the correct outcome — a reader
inventing an inline-marker split would silently disagree with the writer, which
is the class of divergence this spec exists to end. Rewrapping that one header
is filed as **ANTS-3787**, deliberately *after* this change: INV-9's fixture is
derived from its current shape, so fixing the header in the same commit would
delete the evidence that the rule handles it.

**A list bullet — one of `-`, `*`, `+` followed by a space — is not a
terminator.** It is absent from the list above deliberately, and a
bullet-leading line is therefore a continuation unless it independently matches
one of the four. This is measured, not assumed: in
`docs/specs/ANTS-1436.md` the Status field's continuation line begins
`` + `cmdRoadmapQuery` offset/limit; … `` and is prose, not a list item — the
sentence reads ``…`src/paginationengine.{h,cpp}` + `cmdRoadmapQuery`…``. A
terminator set including `^[-*+]\s` truncates it there, and is rejected for
that reason; `tools/spec-header-survey.py` prints whether that evidence is still
in the corpus (`prose-bullet continuation … present`), so a reader can re-check
the claim in one command instead of taking it on trust. Table rows, block
quotes and fences are excluded on the same grounds: no corpus evidence that
they ever terminate a field, and a false terminator silently drops text.

### 2.2 Shared implementation

`src/specparse.{h,cpp}` (`ants_core_lib`, Qt6::Core-only, already the hoisted
home of spec-body parsing):

```cpp
namespace SpecParse {

// The lines one `**Field:**` header entry occupies, per specs.md § 3.2.
struct FieldExtent {
    int     line      = -1;  // 0-based index of the `**Field:**` line; -1 = absent
    int     lineCount = 0;   // 1 + continuation lines
    QString value;           // trailing text + continuations, space-joined
    bool    found() const { return line >= 0; }
};

// First header field named `name` ("Status", "Kind"), searched from the top
// and BOUNDED to the header block: the first `^## ` heading, else EOF. The
// bound is load-bearing -- see § 2.1. Absent field -> found() == false.
FieldExtent headerField(const QStringList &lines, const QString &name);

}  // namespace SpecParse
```

**Two consumers adopt the helper; a third is scoped out with a reason.**
`workspace_search` over `src/` for the regex
`\*\*(Status|Kind):\*\*|statusRe|kindRe` — both field literals and both
identifiers, so a consumer spelling its pattern differently is still caught —
returns six files. Three are prose (MCP tool-description strings in
`claudeintegration.cpp`, a comment in `remotecontrol.cpp`, a comment in
`speclog.h`; the first and third are stale after this change and § 7 carries
them). Of the three that parse:

- `SpecParse::parseSpecBody` replaces `statusRe` / `kindRe` with
  `headerField(...).value`.
- `SpecLog::setStatus` (`src/speclog.cpp`) replaces lines
  `[line, line + lineCount)` with the single line `**Status:** <newStatus>`,
  instead of rewriting `lines[i]` alone.
- `src/docsindex.cpp` carried a **third** copy — `statusRx`, the same
  line-scoped `^\*\*Status:\*\*\s*(.+)$` — feeding `DocEntry::status`. It did
  **not** adopt the helper here, and § 5 said why. *(Superseded: **ANTS-3786**
  shipped 2026-08-02 and folded it in — `docsindex.cpp` now buffers its header
  block and calls `headerField` once. Recorded as superseded rather than
  annotated: a stale "does not adopt" tells a reader the duplication is still
  there, which is worse than silence.)*

**Index conventions differ across the seam, so state the conversion rather than
leaving it to be guessed:** `FieldExtent::line` is **0-based** (it indexes the
`QStringList`), while `EditResult::line` is **1-based** — the return shape
`ANTS-1963` INV-10 pins. `setStatus` therefore returns `extent.line + 1`, the
line of the single rewritten Status line, and `bytes_written` covers the whole
file as before. Neither field reports the continuation lines that were removed.

`speclog.cpp` gains `#include "specparse.h"`; both are already in
`ants_core_lib` (`CMakeLists.txt` — `src/speclog.cpp`, `src/specparse.cpp`).

### 2.3 Standard amendment

`docs/standards/specs.md` § 3.2 gains two normative statements it does not
currently make. The standard is the rule's home; this spec and the code are its
implementations.

1. **A header field's value may wrap**, and it ends per § 2.1's terminator set.
   Today § 3.2 says only "Bold key-value lines immediately under the H1" while
   simultaneously telling authors to "Append cold-eyes progress inline as it
   happens" — guidance that produces wrapping the same section leaves undefined.
2. **One field per line.** A second `**Field:**` marker on the same physical
   line is not a second field; the extent rule reads it as value text (§ 2.1,
   INV-9). Stating it makes `ANTS-1253`'s header a nonconformance with a named
   rule rather than an unspecified shape nobody can call wrong.
3. **A continuation line must not begin with a bold colon-run.** Re-wrap so the
   run sits mid-line, as `ANTS-3766` already does. This is an authoring rule
   because no reader can tell such a line from a new field (§ 2.1), so the
   only place the ambiguity can be removed is where the line break is chosen.
   It costs an author nothing — the run is prose either way — and it is what
   keeps a conforming header from truncating under a rule it satisfies.

## 3. Invariants

- **INV-1** — A header field's extent runs from its `**Field:**` line through
  every following line up to, but excluding, the first blank line, the first
  further field marker, the first ATX heading, or EOF. *Test:*
  `tests/features/spec_field_extent/` — fixtures terminating on each of the
  four, asserting `lineCount`.
- **INV-2** — A bullet-leading continuation line belongs to the field, and the
  value is the opener's trailing text plus each continuation, each stripped and
  joined with exactly one space. *Test:*
  `tests/features/spec_field_extent/` — a fixture holding a byte-for-byte copy
  of `docs/specs/ANTS-1436.md`'s header, checked in so that no later edit to
  that live spec can move the expectation; asserts
  `lineCount == 2` and that the value equals exactly ``shipped 2026-05-16
  (commit f9647bc; `src/paginationengine.{h,cpp}` + `cmdRoadmapQuery`
  offset/limit; tests `tests/features/roadmap_query_pagination/`).`` — the
  literal `tools/spec-header-survey.py --values` prints for that file. Asserting
  the exact joined string is what makes the separator and the strip testable; an
  assertion that merely checks the tail survives a join on `""` or `"\n"`.
- **INV-3** — `SpecParse::parseSpecBody` returns that whole joined value for
  `Status` and `Kind`; no value ends at the first physical line when
  continuations follow. *Test:* `tests/features/spec_field_extent/` — parse the
  INV-2 fixture and assert `status` is the full joined value rather than the
  first-line prefix ending `` `src/paginationengine.{h,cpp}` `` that the
  pre-fix parser returns. Corpus-wide drift is the survey tool's job, not a
  test's: the corpus moves continuously, and the first `set_status` against a
  live wrapped spec — which this change newly enables — would collapse its wrap
  and red a test bound to it.
- **INV-4** — `SpecLog::setStatus` replaces the field's **whole** extent, so
  no continuation line survives the write. *Test:*
  `tests/features/mcp_spec_log/` — a fixture shaped like
  `docs/specs/ANTS-3766-roadmap-migration-archives.md` (Status + 3
  continuations + `**Kind:**`); asserts the output holds exactly one Status
  line and that `**Kind:**` is the next line.
- **INV-5** — `setStatus` changes nothing outside the field: bytes before the
  Status line and after the last continuation are identical, a trailing newline
  is preserved exactly, and the returned `line` is the 1-based line of the
  single rewritten Status line (`extent.line + 1`), per `ANTS-1963` INV-10.
  *Test:* `tests/features/mcp_spec_log/` — byte-compare the head and tail slices
  around the edit, and assert the returned `line`.
- **INV-6** — The two consumers § 2.2 names share one implementation:
  `speclog.cpp` calls `SpecParse::headerField` and carries no field-matching
  regex of its own. Scoped to those two deliberately — an invariant claiming
  the whole library would have been false on the day it shipped.
  *(ANTS-3786 shipped 2026-08-02 and folded in the third consumer,
  `docsindex.cpp`, which now calls `headerField` and `isHeaderBlockEnd` too. It
  carries **its own** scrape in `tests/features/docsindex_header_field/`, so
  this invariant's two-file test surface below is unchanged and still literally
  true. A reader should not conclude the rule has exactly two consumers.)*
  *Test:* source scrape in
  `tests/features/spec_field_extent/`, asserting **both** files: `speclog.cpp`
  and `specparse.cpp` each contain `headerField(`, and neither contains
  `statusRe` or `kindRe`. Scraping only `speclog.cpp` would pass a build that
  fixed `parseSpecBody` by widening its *own* regex — which satisfies INV-3
  behaviourally while leaving the second copy of the rule in place, the exact
  outcome this invariant exists to reject.
  The positive half is load-bearing — an absence assertion alone passes against
  a hand-rolled `startsWith("**Status:**")` that never adopted the shared rule,
  which is the one implementation this invariant exists to reject. **The
  absence half greps the identifier, not the field literal**, because a correct
  `speclog.cpp` still contains `**Status:**` twice and must: once as the
  replacement line § 2.2 mandates, once in the `unrecognised_format` message
  INV-7 keeps green. A scrape aimed at the literal would fail against the right
  answer. `tools/spec-header-survey.py` is out of this invariant's scope (§ 7).
- **INV-7** — A document with no Status line is still `unrecognised_format`,
  and a `status` argument that is empty is still `bad_args`; this change adds
  no refusal code. *Test:* the existing `tests/features/mcp_spec_log/` T6
  refusal cases, unchanged and still green.
- **INV-8** — `Kind` is governed by the same rule as `Status` — one
  implementation, not a Status special case. *Test:*
  `tests/features/spec_field_extent/` — a `**Kind:**` fixture with one
  continuation line, asserting the joined value; the fixture is synthetic
  because the corpus's only wrapped `Kind` is also its inline-marker case
  (INV-9), and a fixture exercising two rules at once proves neither.
- **INV-9** — A `**Field:**` marker begins or ends a field only at the start
  of a line; a bold colon-run inside a line is value text. *Test:*
  `tests/features/spec_field_extent/` — the `docs/specs/ANTS-1253.md` shape
  (`**Kind:** … **Lanes:** …` on one physical line, one continuation line),
  asserting the returned `Kind` retains the literal `**Lanes:**` rather than
  stopping at it, and that `lineCount` is 2.
- **INV-10** — The search is bounded to the header block, so a `**Status:**`
  line inside a fenced example below the first `^##\s` heading is never matched;
  a document whose only such line is fenced is still `unrecognised_format`.
  *(ANTS-3786 moved the `^##\s` test out of a private `blockEndRe` inside
  `headerField` into the exported `SpecParse::isHeaderBlockEnd`, so a reader of
  § 2.1 no longer finds the regex there. Behaviour is identical — the extraction
  was required not to change it — and this invariant holds verbatim.)*
  *Test:* one fixture, two homes, matching the § 6 split — a document with no
  header Status and a fenced `**Status:** spec draft (YYYY-MM-DD).` in a later
  section (the shape `docs/standards/specs.md` § 3.2 itself has).
  `tests/features/spec_field_extent/` asserts `headerField` reports absent;
  `tests/features/mcp_spec_log/` asserts `setStatus` returns
  `unrecognised_format` rather than rewriting inside the fence — the same home
  as INV-7's other refusal cases. Against an unbounded scan the fixture is
  rewritten in-fence and both legs fail, which is the mutation that makes this
  non-vacuous.

## 4. RAM / build cost

No new build target, no new library, no external dependency. `headerField` is a
pure function over a `QStringList`, and **the two callers reach it
differently**: `SpecLog::setStatus` already holds one (`toLines()`), while
`SpecParse::parseSpecBody` matches `MultilineOption` regexes against the whole
`QString body` and holds no line list at all — so it gains one
`body.split('\n')`. That is one allocation of the document's lines per parse, on
a path that already scans the same text several times for title, metadata and
the invariants section.

`headerField` itself is O(header-block lines) — bounded by the block, not the
document — and runs twice per parse (`Status`, `Kind`). Beyond the split it
allocates one `FieldExtent` and the joined value, both scoped to the call. No
cache, no state, so no eviction policy is needed. Build cost is one added
`#include "specparse.h"` in a file already in the same library.

## 5. Out of scope

- **Re-wrapping the written value to 80 columns.** `setStatus` writes the
  caller's text as one line; a long status stays long until an author wraps it.
  Wrapping on write would reformat prose the caller did not ask to reformat.
- **Full fence-masking *inside* the header block.** The header-block bound
  (§ 2.1, INV-10) closes the fence hole at the boundary; masking would close it
  *within* the block, and nothing in the corpus needs that — the survey tool
  reports `fence opened inside a header block: 0`, which is the figure that
  actually measures this (`first field inside a fence: 0` measures something
  narrower and is not offered as evidence for it). Should a document ever open a
  fence in its header block, `MarkdownScan::fenceMask` is the ready primitive
  and both consumers would adopt it together.
- **Repairing already-corrupted specs.** The survey tool reports
  `orphaned-continuation signature: 0` — no spec currently carries the shape a
  first-line-only rewrite leaves behind. `ANTS-3766` was repaired by hand on
  2026-08-01. This change is therefore preventive on the write side and
  corrective on the read side, with no repair pass to run.
- **`src/docsindex.cpp`'s third copy of the rule — ANTS-3786.** It streams the
  file line by line under a per-doc byte budget (its own INV-19) and holds no
  `QStringList`, so adopting `headerField` meant buffering to the header-block
  bound — a change to a different spec's invariant, made under a different
  spec's contract. Folding it in here would have widened a two-file fix into a
  three-spec one. It was named rather than left silent because a third copy is
  exactly what INV-6 exists to prevent, and scoping it out without saying so
  would make INV-6 read as achieved when it is achieved only for the two
  consumers listed in § 2.2. *(Superseded: ANTS-3786 shipped 2026-08-02 and did
  exactly that, including exporting `SpecParse::isHeaderBlockEnd` so the block
  bound is shared rather than re-expressed. This bullet is kept in the past
  tense because it records why the split was made, not an open exclusion.)*
- **Detecting a continuation line that begins with a bold colon-run** (§ 2.1's
  sharp edge). It is an authoring rule in § 2.3, and enforcing it belongs to
  `spec_lint`, whose job is the greppable half of the spec-format contract —
  not to a parser that by construction cannot tell the two apart.
- **The 49 wrapped specs' text — measured, not assumed.** The claim that they
  are well-formed under this rule needs evidence the wrapped-count cannot give:
  a spec whose continuation line *begins* with a bold colon-run is read as a
  separate field, so it never enters the 49 and no count of wrapped fields can
  ever surface it. The tool therefore also censuses **every** header-field name
  and flags the prose-shaped ones — currently `46 (4 prose-shaped)`, and all
  four were opened and confirmed to be deliberate, if verbose, field names
  (`Builds on (verified enablers, 2026-07-20)`, `Cold-eyes pass 2 hardening`,
  `Complements (does not duplicate)`, `Hard-depends on ANTS-1893 landing
  first`), each correctly terminating the wrapped field above it. So no corpus
  spec is mis-split today, and the census is how the next reader re-checks
  rather than re-assumes. The code is what changes.
- Renaming bare-id spec files — ANTS-3755.

## 6. Tests

New feature test `tests/features/spec_field_extent/`, covering INV-1, INV-2,
INV-3, INV-6, INV-8, INV-9 and INV-10's reader leg. Extensions to the existing
`tests/features/mcp_spec_log/` cover INV-4, INV-5, INV-7 and INV-10's writer
leg — every `setStatus` assertion lives there, so the refusal cases stay in one
place. Every fixture is
checked in — no test reads a live spec, so corpus edits cannot red the suite.
Label
`features;fast`; sources join an existing bundle's `SOURCES` list per
`tests/features/README.md` rather than adding an `add_executable`.

Per project convention each test is verified to **fail against pre-fix
source** before the fix is restored — for INV-3 and INV-4 that failure is the
defect itself (a truncated value; an orphaned continuation line).

`tests/features/mcp_spec_query/` INV-5b/5c/5d scrape `src/specparse.cpp` by
whole-file `contains()`, not by byte window, so adding `headerField` cannot
move an anchor; the three asserted literals are untouched by this change.

## 7. Cross-doc impact

- `docs/standards/specs.md` § 3.2 — the extent rule (§ 2.3 above). Gated
  separately as a standard.
- `docs/specs/ANTS-1963.md` INV-5 — currently reads *"rewrites the first
  `^\*\*Status:\*\*` line's trailing text … and changes nothing else"*, which
  mandates the defect. Amended to the whole-extent contract, citing this spec.
  Per `specs.md` § 5.5 the id is amended in place, never renumbered.
- `tools/spec-header-survey.py` — new, ships with this change. It produces every
  corpus figure above, and `--values` prints each wrapped field's `line_count`
  and joined value, which are exactly the two outputs `headerField()` must
  reproduce — so it is a parity oracle for those two, not for the whole struct.
  It carries its own copy of the extent rule on purpose (INV-6): an oracle that
  imported the implementation under test could not disagree with it. **Not a
  build target and not a gate** — it exits 0 and reports. It is run by hand,
  author-side, whenever the extent rule or its corpus evidence changes; nothing
  runs it automatically, so it makes the figures re-derivable rather than
  self-maintaining.
- `src/speclog.h` — the `EditResult` comment reads "replace the text after the
  first `**Status:**` line", which describes the defect. Update with the code.
- `tests/features/mcp_spec_log/` — T1's comment reads "set_status rewrites only
  the Status line". Its assertions still pass (its fixture's Status is
  single-line), but the sentence is now wrong; it is reworded in the same
  commit. `src/remotecontrol.cpp:17505`'s comment was checked and is **not**
  stale — it says where Status comes from, not how much of it is rewritten.
- `src/claudeintegration.cpp` — the `spec_log` MCP tool-description strings say
  `set_status` rewrites "the text after `**Status:** `" and "the **Status:**
  line". Both describe first-line-only behaviour to every caller of the verb,
  so both change in this commit; the schema text is the contract a caller reads.
- `ROADMAP.md` — ANTS-3785 and ANTS-3672 both flip on ship; ANTS-3672 lives in
  a different section (`ants-mcp-improvements-…-2026-05-14`). **ANTS-3786** is
  filed by this spec and stays open.
- `CHANGELOG.md` — one `Fixed` entry naming both ids.
- No `CLAUDE.md` or `README.md` change: neither documents the header-field
  grammar.

## 8. Cold-eyes loop log

| Loop | Reviewer | Findings | Resolution |
|---|---|---|---|
| 3-impl (2026-08-02) | none — implementation, not a review | contract held; 2 corrections | **Implementation row, written by the implementer** (`/write-spec` step 8); no reviewer was dispatched. Built test-first: the two writer tests were run RED against pre-fix code and **both failed exactly as specified** — `SetStatusReplacesWholeWrappedExtent` found the orphaned `[ANTS-3782](…)` continuation where `**Kind:**` should be, and `SetStatusIgnoresFencedStatusBelowHeaderBlock` returned `ok:true`, i.e. the pre-fix writer **rewrote a `**Status:**` inside a code fence and reported success.** INV-10 was written from a contract gap with no corpus instance; it turns out to be a live defect in code, which is the strongest evidence in the run that the bound is not bookkeeping. Then 24/24 green (10 new `SpecFieldExtent` + 14 `McpSpecLog`). **Two things the contract got wrong and building it exposed.** (1) **§ 2.2's signature is right but its cost note was for the wrong caller**: `parseSpecBody` takes `QString body` and had no `QStringList`, so the split § 4 prices is real and lands in the reader, not the writer — already corrected at loop 2, and confirmed here by the compiler rather than by reading. (2) **The INV-6 scrape needed the identifier, not the literal**, and this is now demonstrated rather than argued: the shipped `speclog.cpp` still contains `**Status:**` twice (`speclog.cpp` writes the replacement line and the `unrecognised_format` message), so a literal-based absence assertion would have reddened the correct fix. Also corrected in flight: the test's `srcgrep.h` include needed the `../../_support/` prefix every sibling uses — caught because a `\| tail` pipeline had masked ninja's exit status and the first "green" run was against a **stale binary**, the inverse of the stale-binary false pass. No contract clause was found false; the amendments were to § 4's premise and INV-6's test surface, both already folded. |
| 3 (2026-08-02) — **converged at the cap; ratio trigger also fired** | 1 cold `general-purpose` lane, same byte-identical packet, no prior-loop context | C 0 · H 2 · M 3 · L 4 · I 1 — 10 verified, 0 dismissed | All 10 fixed; nothing deferred. **No loop-1 or loop-2 finding resurfaced in either subsequent pass**, which is the evidence those fixes held. **H1 — § 5's "the 49 wrapped specs are well-formed" was unevidenced, and the tool was structurally blind to its counterexample**: a spec whose continuation line *begins* with a bold colon-run is read as a separate field, so it never enters the wrapped count and no count of wrapped fields could ever surface it. The tool now censuses every header-field name and flags the prose-shaped ones — `46 (4 prose-shaped)` — and **all four were opened and confirmed** deliberate, if verbose, field names, each correctly terminating the wrapped field above it. So the claim is true, and is now re-checkable instead of assumed. **H2 — INV-6's headline claimed two consumers while its test scraped one.** A build that fixed `parseSpecBody` by widening its *own* regex would satisfy INV-3 behaviourally and pass INV-6 by construction, leaving the second copy in place — the outcome the invariant exists to reject. The scrape now asserts both files. **M1 — the fence justification rested on a figure measuring something else** (`first field inside a fence` is not `fence opened inside a header block`); the tool now emits the figure the claim actually needs, and § 5 no longer offers the narrower one as evidence. **M2 — § 2.3 turned `ANTS-1253`'s header into a named nonconformance with no id filed**, unlike `docsindex.cpp`; now **ANTS-3787**, sequenced deliberately *after* this change because INV-9's fixture is derived from that header's current shape. Also verified and **not** changed: `remotecontrol.cpp:17505`'s comment is not stale (it says where Status comes from, not how much is rewritten). Origin split: 4 draft defects vs 5 fix collateral — **the second consecutive loop where collateral leads, which is Phase 5's ratio trigger**, and it coincides with the `--max-loops 3` cap. Both say stop, and nothing verified remains unfixed, so the run exits converged rather than deferring a tail. Doc grew 411 → 439 lines (253 at loop 1). |
| 2 (2026-08-02) | 1 cold `general-purpose` lane, same byte-identical packet, no prior-loop context | C 0 · H 1 · M 4 · L 4 · I 2 — 9 verified, 0 dismissed, **plus 1 found in verification** | **No loop-1 finding resurfaced, which is the evidence those fixes held.** All 9 fixed. **The largest item was not the lane's**: verifying its MEDIUM about the § 2.2 consumer search — that `Status:\*\*|statusRe` could not catch a `Kind`-only consumer — meant re-running the search widened to `\*\*(Status\|Kind):\*\*\|statusRe\|kindRe`, which returns **six** files rather than two. `src/docsindex.cpp:82` is a **third implementation** of the rule (`statusRx`, line-scoped, same truncation defect), feeding `DocEntry::status`. So § 2.2's headline claim "Two consumers, no third" was false, and INV-6's "one implementation in `ants_core_lib`" would have been false the day it shipped. Corrected, scoped out with its reason, and filed as **ANTS-3786** rather than folded in — `docsindex` streams under a per-doc byte budget (its own INV-19) and holds no `QStringList`, so adopting the helper is a change under a different spec's contract. Two prose sites were also stale and now ship with the code: `speclog.h`'s `EditResult` comment and `claudeintegration.cpp`'s `spec_log` tool-description strings, which describe first-line-only behaviour to every caller of the verb. **H1 (lane) — the rule's one sharp edge**: field extent depends on where the author's line wrap lands, since a continuation line beginning with a bold colon-run is *indistinguishable* from a new field; `ANTS-3766`'s own `**Split at loop 4:**` sits mid-line by luck and would truncate its Status one word earlier. Not fixable in the reader (the field-name vocabulary is open — the corpus carries `Lanes`, `Siblings`, `Applies to` beyond § 3.2's ten), so it becomes § 2.3 authoring rule 3, with detection scoped to `spec_lint`. **M1 — § 4's cost claim rested on an unverified premise**: `headerField` was said to be "a pure function over a `QStringList` already held by both callers", but `specparse.cpp` contains no `QStringList` and no `.split(` at all — it matches `MultilineOption` regexes against the whole body — so the reader gains a split that § 4 now prices, with a complexity bound. **M2 — INV-6's scrape would have reddened a correct implementation**: a correct `speclog.cpp` still contains `**Status:**` twice and must (the replacement line § 2.2 mandates, and the refusal message INV-7 keeps green), so the absence assertion now greps the identifier `statusRe` instead of the field literal. Origin split: 4 draft defects vs 6 fix collateral — collateral leads for the first time, so the Phase 5 ratio trigger is armed but not met (it needs two consecutive loops). Doc grew 343 → 411 lines. |
| 1 (2026-08-02) | 2 cold `general-purpose` lanes, one shared byte-identical packet, no prior-loop context | C 0 · H 4 · M 7 · L 4 · I 1 — 15 verified, 0 dismissed | All 15 fixed; the INFO (no TOC at >200 lines) dismissed with reason — `specs.md` § 7's skeleton has no TOC slot and no sibling spec carries one. **Both lanes independently reported the two largest.** **H4 — the field search was unbounded**, and § 5's justification for skipping fence-awareness ("structurally impossible") was false as stated: it holds only for a document that *has* the field, while INV-7's no-Status path scans to EOF and can match a `**Status:**` inside a fenced example — `docs/standards/specs.md` § 3.2 contains exactly that shape, and `spec_log`'s `path` routing admits any in-repo file, so the writer could have rewritten inside a code fence and returned `ok`. Now bounded to the header block, with **INV-10** and a fixture whose mutation (an unbounded scan) reddens it. *Measured:* no corpus document triggers it today (`orphaned-continuation signature: 0`, `first field inside a fence: 0`), so this was a contract gap rather than a live defect. **H3 — INV-6 asserted only the ABSENCE of a regex**, which passes against a hand-rolled `startsWith("**Status:**")` that never adopted the shared rule; paired with a positive `SpecParse::headerField(` assertion, and its "exactly one implementation" claim scoped to `ants_core_lib` since the survey tool deliberately carries its own copy. **H2 — INV-3's expected literal was wrong**: it dropped a backtick from ANTS-1436's tail, so the clause could not have passed as written; the fixture is now checked in and the literal is what `--values` prints. **H1 — INV-2's bullet rule contradicted INV-1**: `**Kind:**` begins with `*`, and terminator precedence was unstated, so an implementer could have built a parser that swallowed every following field. **M1 was the subtlest: INV-3 bound a permanent test to a live corpus file that this very change would invalidate** — the first `set_status` against ANTS-1436 collapses its wrap — so every fixture is now checked in. Also: `**Amends:**` was an invented relationship line (grep: used in this file alone) and § 3.2 never actually said "one field per line", which § 2.3 now adds rather than assuming; the join separator and the 0-based/1-based seam were unasserted; and the orphan-corruption figure was prose-only, so the scan moved into the tool. Doc grew 253 → 343 lines. | 
