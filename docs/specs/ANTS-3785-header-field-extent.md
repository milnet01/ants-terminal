# ANTS-3785 — Spec header fields may wrap: one extent rule, one implementation

**Status:** spec draft (2026-08-02).
**Kind:** fix.
**Source:** ROADMAP.md ANTS-3785 (hit 2026-08-01 while shipping ANTS-3766) and
ANTS-3672 (found 2026-07-28 verifying ANTS-3665 against the live corpus).
**Covers:** ANTS-3785 (writer corrupts a wrapped field), ANTS-3672 (reader
truncates a wrapped field).
**Pairs with:** `docs/standards/specs.md` § 3.2 (header block) — the normative
home of the rule this spec implements.
**Amends:** ANTS-1963 INV-5, which mandates the defective behaviour.

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

**Scale.** 49 of 172 specs carrying a Status field have a wrapped one — every
one of them read wrong today, and every one a corruption waiting for the next
`set_status`. `Kind` wraps once in 149.

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
# prose-bullet continuation (ANTS-1436.md): present
```

**Why one spec for two ids.** The two defects are one missing rule with two
faces. Fixing either alone means writing "where does a header field end" once
and then writing it again — the divergence `MarkdownScan` (ANTS-3603) and
`SpecParse` (ANTS-3665) were both hoisted to prevent. `specs.md` § 2 permits
one umbrella spec whose header lists the ids it covers; this is that.

## 2. Surface

### 2.1 The extent rule (normative)

A header field begins on a line matching `^\*\*(?<name>[^*:]+):\*\*` and
extends through every following line until the first line that is **any** of:

- blank (`^\s*$`),
- another field marker (`^\*\*[^*:]+:\*\*`),
- an ATX heading (`^#{1,6}\s`) — headings may interrupt a paragraph, so a
  header block written without its blank separator must not swallow § 1,
- end of file.

The field's **value** is its own trailing text plus each continuation line,
each stripped of surrounding whitespace and joined with a single space.

**A continuation line may begin with a list-bullet character, and bullets do
not terminate a field.** This is measured, not assumed: in
`docs/specs/ANTS-1436.md` the Status field's continuation line begins
`` + `cmdRoadmapQuery` offset/limit; … `` and is prose, not a list item — the
sentence reads ``…`src/paginationengine.{h,cpp}` + `cmdRoadmapQuery`…``. A
terminator set including `^[-*+]\s` truncates it there, and is rejected for
that reason; `tools/spec-header-survey.py` reports whether that evidence is
still in the corpus rather than leaving this claim to rot. Table rows, block
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

// First header field named `name` ("Status", "Kind"). Scans from the top;
// the header block is by definition the first bold key-values under the H1.
FieldExtent headerField(const QStringList &lines, const QString &name);

}  // namespace SpecParse
```

Two consumers, no third (`workspace_search` for `statusRe` across `src/`
returns `specparse.cpp` and `speclog.cpp` only):

- `SpecParse::parseSpecBody` replaces `statusRe` / `kindRe` with
  `headerField(...).value`.
- `SpecLog::setStatus` (`src/speclog.cpp`) replaces lines
  `[line, line + lineCount)` with the single line `**Status:** <newStatus>`,
  instead of rewriting `lines[i]` alone.

`speclog.cpp` gains `#include "specparse.h"`; both are already in
`ants_core_lib` (`CMakeLists.txt` — `src/speclog.cpp`, `src/specparse.cpp`).

### 2.3 Standard amendment

`docs/standards/specs.md` § 3.2 gains the extent rule as normative prose. The
standard is the rule's home; this spec and the code are its implementations.

## 3. Invariants

- **INV-1** — A header field's extent runs from its `**Field:**` line through
  every following line up to, but excluding, the first blank line, the first
  further field marker, the first ATX heading, or EOF. *Test:*
  `tests/features/spec_field_extent/` — fixtures terminating on each of the
  four, asserting `lineCount`.
- **INV-2** — A continuation line beginning with a list-bullet character
  (`-`, `*`, `+`) belongs to the field; bullets do not terminate it. *Test:*
  `tests/features/spec_field_extent/` — fixture taken verbatim from
  `docs/specs/ANTS-1436.md` lines 3–4; asserts the value retains the text after
  the `+`.
- **INV-3** — `SpecParse::parseSpecBody` returns the whole joined value for
  `Status` and `Kind`; no value ends at the first physical line when
  continuations follow. *Test:* `tests/features/spec_field_extent/`, plus a
  live assertion that `spec_query` on `docs/specs/ANTS-1436.md` returns a
  status ending `roadmap_query_pagination/).` rather than
  `` `src/paginationengine.{h,cpp}` ``.
- **INV-4** — `SpecLog::setStatus` replaces the field's **whole** extent, so
  no continuation line survives the write. *Test:*
  `tests/features/mcp_spec_log/` — a fixture shaped like
  `docs/specs/ANTS-3766-roadmap-migration-archives.md` (Status + 3
  continuations + `**Kind:**`); asserts the output holds exactly one Status
  line and that `**Kind:**` is the next line.
- **INV-5** — `setStatus` changes nothing outside the field: bytes before the
  Status line and after the last continuation are identical, and a trailing
  newline is preserved exactly. *Test:* `tests/features/mcp_spec_log/` —
  byte-compare the head and tail slices around the edit.
- **INV-6** — Exactly one implementation of the rule exists: `speclog.cpp`
  carries no `Status:`-matching regex of its own. *Test:* source scrape in
  `tests/features/spec_field_extent/`, asserting `src/speclog.cpp` does not
  contain `Status:\\*\\*` inside a `QRegularExpression`. Breaks if a future
  edit re-inlines the pattern.
- **INV-7** — A document with no Status line is still `unrecognised_format`,
  and a `status` argument that is empty is still `bad_args`; this change adds
  no refusal code. *Test:* the existing `tests/features/mcp_spec_log/` T6
  refusal cases, unchanged and still green.
- **INV-8** — `Kind` is governed by the same rule as `Status` — one
  implementation, not a Status special case. *Test:*
  `tests/features/spec_field_extent/` — the `docs/specs/ANTS-1253.md` shape
  (the corpus's one wrapped `Kind`), asserting the joined value.

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
- **Fence-awareness for the field scan.** `tools/spec-header-survey.py` reports
  `first field inside a fence: 0`, and it is structurally impossible anyway:
  § 3.2 puts the header block immediately under the H1, so no fence can precede
  it. Should that ever change, `MarkdownScan::fenceMask` is the ready primitive.
- **Repairing already-corrupted specs.** No spec currently carries the
  orphaned-fragment signature — `ANTS-3766` was repaired by hand on 2026-08-01
  and a scan for a short sentence-final Status followed by a lowercase
  continuation returns nothing. This change is preventive on the write side and
  corrective only on the read side.
- **The 49 wrapped specs' text.** They are well-formed under this rule; the
  code is what changes.
- Renaming bare-id spec files — ANTS-3755.

## 6. Tests

New feature test `tests/features/spec_field_extent/`, covering INV-1, INV-2,
INV-3, INV-6 and INV-8. Extensions to the existing
`tests/features/mcp_spec_log/` cover INV-4, INV-5 and INV-7. Label
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
- `tools/spec-header-survey.py` — new, ships with this change. The parity
  oracle for `headerField()` and the producer of every corpus figure above.
  Not a build target and not a gate; it exits 0 and reports.
- `ROADMAP.md` — ANTS-3785 and ANTS-3672 both flip on ship; ANTS-3672 lives in
  a different section (`ants-mcp-improvements-…-2026-05-14`).
- `CHANGELOG.md` — one `Fixed` entry naming both ids.
- No `CLAUDE.md` or `README.md` change: neither documents the header-field
  grammar.

## 8. Cold-eyes loop log

<!-- Rows are written by /cold-eyes as each loop closes. -->
