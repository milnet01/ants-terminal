# ANTS-3785 — Spec header fields may wrap: one extent rule, one implementation

**Status:** spec draft (2026-08-02).
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
# orphaned-continuation signature: 0
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
exists to end. The bound is what makes INV-7's `unrecognised_format` reachable
instead of accidentally satisfied.

Within that block, a field begins on a line matching
`^\*\*(?<name>[^*:]+):\*\*` and extends through every following line until the
first line that is **any** of:

- blank (`^\s*$`),
- another field marker (`^\*\*[^*:]+:\*\*`),
- an ATX heading (`^#{1,6}\s`) — headings may interrupt a paragraph, so a
  header block written without its blank separator must not swallow § 1,
- end of file.

These four are the **extent** terminators; the header-block bound above is a
separate, earlier constraint on *where a field is looked for at all*. They
coincide in practice — the `^##\s` that ends the block is also an ATX heading —
and they are stated apart because INV-1 tests the terminators and INV-10 tests
the bound.

**A line terminates the field if it matches ANY of those; it is a continuation
only if it matches NONE.** That ordering is the whole rule, and it is what
settles the `*` ambiguity below: `**Kind:** implement.` begins with `*`, but it
matches the field-marker terminator, so it ends the field — it is not rescued by
the bullet rule.

The field's **value** is its own trailing text plus each continuation line,
each stripped of surrounding whitespace and joined with a single space.

**Only a marker at the START of a line opens or closes a field.** A bold
run ending in a colon *within* a line is ordinary value text — every anchor
above is `^`-anchored for this reason. Two corpus cases pin it:
`docs/specs/ANTS-3766-roadmap-migration-archives.md` carries
`**Split at loop 4:**` inside its Status prose, and `docs/specs/ANTS-1253.md`
packs `**Lanes:**` onto the same physical line as its `**Kind:**` value.

The second packs two fields onto one physical line. `specs.md` § 3.2 does not
currently forbid that — it says only "Bold key-value lines immediately under the
H1" — so § 2.3's amendment adds the one-field-per-line requirement that the
extent rule depends on. Until a header is rewritten to satisfy it, this rule
reads it faithfully rather than repairing it: `Kind` in `ANTS-1253` returns the
whole logical run, `**Lanes:**` included. That is the correct outcome — a reader
inventing an inline-marker split would silently disagree with the writer, which
is the class of divergence this spec exists to end. Rewriting that one header is
`ANTS-1253`'s business, not this rule's.

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

Two consumers, no third — `workspace_search` over `src/` for the regex
`Status:\*\*|statusRe` (the field literal **or** the identifier, so a consumer
that spells its pattern differently is still caught) returns only
`specparse.cpp` and `speclog.cpp`:

- `SpecParse::parseSpecBody` replaces `statusRe` / `kindRe` with
  `headerField(...).value`.
- `SpecLog::setStatus` (`src/speclog.cpp`) replaces lines
  `[line, line + lineCount)` with the single line `**Status:** <newStatus>`,
  instead of rewriting `lines[i]` alone.

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
- **INV-6** — One implementation of the extent rule ships in `ants_core_lib`:
  `speclog.cpp` calls `SpecParse::headerField` and carries no field-matching
  regex of its own. *Test:* source scrape in
  `tests/features/spec_field_extent/`, asserting `src/speclog.cpp` **does**
  contain `SpecParse::headerField(` **and does not** contain `Status:\\*\\*`.
  The positive half is load-bearing — an absence assertion alone passes against
  a hand-rolled `startsWith("**Status:**")` that never adopted the shared rule,
  which is the one implementation this invariant exists to reject.
  `tools/spec-header-survey.py` deliberately carries its own copy of the rule
  and is out of this invariant's scope: an oracle that shared the
  implementation under test would prove nothing.
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
  *Test:* `tests/features/spec_field_extent/` — a fixture with no header Status
  and a fenced `**Status:** spec draft (YYYY-MM-DD).` in a later section (the
  shape `docs/standards/specs.md` § 3.2 itself has); asserts `headerField`
  reports absent and that `setStatus` refuses rather than rewriting inside the
  fence. Against an unbounded scan this fixture is rewritten in-fence and the
  test fails, which is the mutation that makes it non-vacuous.

## 4. RAM / build cost

No new build target, no new library, no external dependency. `headerField` is
a pure function over a `QStringList` already held by both callers; it allocates
one `FieldExtent` and the joined value string, both scoped to the call. No
cache, no state, so no eviction policy is needed. Build cost is one added
`#include "specparse.h"` in a file already in the same library.

## 5. Out of scope

- **Re-wrapping the written value to 80 columns.** `setStatus` writes the
  caller's text as one line; a long status stays long until an author wraps it.
  Wrapping on write would reformat prose the caller did not ask to reformat.
- **Full fence-masking *inside* the header block.** The header-block bound
  (§ 2.1, INV-10) is what closes the fence hole in practice, and it is cheaper
  and more predictable than masking: a fence cannot open and close above the
  first `^##\s` heading in any conforming spec, and the survey tool reports
  `first field inside a fence: 0` across the corpus. Should a document ever need
  true fence-awareness here, `MarkdownScan::fenceMask` is the ready primitive
  and both consumers would adopt it together.
- **Repairing already-corrupted specs.** The survey tool reports
  `orphaned-continuation signature: 0` — no spec currently carries the shape a
  first-line-only rewrite leaves behind. `ANTS-3766` was repaired by hand on
  2026-08-01. This change is therefore preventive on the write side and
  corrective on the read side, with no repair pass to run.
- **The 49 wrapped specs' text.** They are well-formed under this rule; the
  code is what changes.
- Renaming bare-id spec files — ANTS-3755.

## 6. Tests

New feature test `tests/features/spec_field_extent/`, covering INV-1, INV-2,
INV-3, INV-6, INV-8, INV-9 and INV-10. Extensions to the existing
`tests/features/mcp_spec_log/` cover INV-4, INV-5 and INV-7. Every fixture is
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
- `ROADMAP.md` — ANTS-3785 and ANTS-3672 both flip on ship; ANTS-3672 lives in
  a different section (`ants-mcp-improvements-…-2026-05-14`).
- `CHANGELOG.md` — one `Fixed` entry naming both ids.
- No `CLAUDE.md` or `README.md` change: neither documents the header-field
  grammar.

## 8. Cold-eyes loop log

| Loop | Reviewer | Findings | Resolution |
|---|---|---|---|
| 1 (2026-08-02) | 2 cold `general-purpose` lanes, one shared byte-identical packet, no prior-loop context | C 0 · H 4 · M 7 · L 4 · I 1 — 15 verified, 0 dismissed | All 15 fixed; the INFO (no TOC at >200 lines) dismissed with reason — `specs.md` § 7's skeleton has no TOC slot and no sibling spec carries one. **Both lanes independently reported the two largest.** **H4 — the field search was unbounded**, and § 5's justification for skipping fence-awareness ("structurally impossible") was false as stated: it holds only for a document that *has* the field, while INV-7's no-Status path scans to EOF and can match a `**Status:**` inside a fenced example — `docs/standards/specs.md` § 3.2 contains exactly that shape, and `spec_log`'s `path` routing admits any in-repo file, so the writer could have rewritten inside a code fence and returned `ok`. Now bounded to the header block, with **INV-10** and a fixture whose mutation (an unbounded scan) reddens it. *Measured:* no corpus document triggers it today (`orphaned-continuation signature: 0`, `first field inside a fence: 0`), so this was a contract gap rather than a live defect. **H3 — INV-6 asserted only the ABSENCE of a regex**, which passes against a hand-rolled `startsWith("**Status:**")` that never adopted the shared rule; paired with a positive `SpecParse::headerField(` assertion, and its "exactly one implementation" claim scoped to `ants_core_lib` since the survey tool deliberately carries its own copy. **H2 — INV-3's expected literal was wrong**: it dropped a backtick from ANTS-1436's tail, so the clause could not have passed as written; the fixture is now checked in and the literal is what `--values` prints. **H1 — INV-2's bullet rule contradicted INV-1**: `**Kind:**` begins with `*`, and terminator precedence was unstated, so an implementer could have built a parser that swallowed every following field. **M1 was the subtlest: INV-3 bound a permanent test to a live corpus file that this very change would invalidate** — the first `set_status` against ANTS-1436 collapses its wrap — so every fixture is now checked in. Also: `**Amends:**` was an invented relationship line (grep: used in this file alone) and § 3.2 never actually said "one field per line", which § 2.3 now adds rather than assuming; the join separator and the 0-based/1-based seam were unasserted; and the orphan-corruption figure was prose-only, so the scan moved into the tool. Doc grew 253 → 343 lines. | 
