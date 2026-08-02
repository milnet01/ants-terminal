# ANTS-3786 — Read docsindex's Status field with the shared header-field rule

**Status:** spec draft (2026-08-02).
**Kind:** fix.
**Source:** ROADMAP.md ANTS-3786 (in-session-2026-08-02; found in ANTS-3785
cold-eyes loop 2, while verifying that spec's own "two consumers, no third"
claim — the claim was false because the search behind it was too narrow).
**Blocked by:** ANTS-3785 (shipped 2026-08-02; owns `SpecParse::headerField`,
the rule this adopts).
**Pairs with:** ANTS-2139 (owns `docs_index`; its INV-17 and INV-19 are
amended here — § 7).

**Layman:** A third piece of code reads a document's status line and, like the
two already fixed, stops at the first line — so a status written across two
lines is cut short in the documentation index.

**Sections:** [1 Problem](#1-problem) · [2 Surface](#2-surface) ·
[3 Invariants](#3-invariants) · [4 RAM / build cost](#4-ram--build-cost) ·
[5 Out of scope](#5-out-of-scope) · [6 Tests](#6-tests) ·
[7 Cross-doc impact](#7-cross-doc-impact)

## 1. Problem

`src/docsindex.cpp::scanDoc()` carries its own `**Status:**` regex, matched
per line inside the streaming read loop, and keeps the first match's captured
tail. It is the ANTS-3672 defect verbatim: a status whose value wraps onto a
following line is truncated at its first physical line. ANTS-3672 fixed this
in `spec_query`, and ANTS-3785 generalised the rule into
`SpecParse::headerField()`; `docsindex` was scoped out of that change and named
here (ANTS-3785 § 5).

Three consequences, in the order they matter:

1. **Truncated status.** `docs_index` misreports the status of **54** of the
   **178** in-scope documents that carry a `**Status:**` line — **52**
   truncated at their first physical line, and **2** sourced from body prose
   (consequence 3). It feeds
   `DocEntry::status`, which `src/docsindex.h::DocEntry` documents as the
   "best-effort `**Status:**` value" (ANTS-2139 INV-17), so every consumer of
   the index sees the truncation.
2. **A third copy of the rule.** ANTS-3785 INV-6 requires its two named
   consumers to share one implementation. A third file carrying its own copy
   is what that invariant exists to prevent, and it drifts independently —
   this copy never received the ANTS-3672 fix the other two did.
3. **The match is unbounded.** `scanDoc` takes the first `**Status:**` line
   *anywhere in the file*, where `headerField` bounds its search to the header
   block. **2** documents therefore draw a "status" from body prose:
   `ROADMAP.md` (a distribution bullet) and
   `docs/journal/2026-04-13-DISCOVERY.md` (a mid-document phase note). Neither
   is a document status; both are reported as one today.

**Every figure this spec quotes comes from the one command below**, run against
`docs_index`'s real scan scope (`<root>/*.md` non-recursive, then
`<root>/docs/**/*.md` recursive — the walk `src/docsindex.cpp::walkDocs()`
performs), on 2026-08-02. It emits the full cross-tabulation rather than four
totals, so § 2.3's disjoint classes are read off it directly instead of being
derived by subtraction:

```bash
python3 - <<'PY'
import glob, re
docs = sorted(glob.glob('*.md')) + sorted(glob.glob('docs/**/*.md', recursive=True))
S = re.compile(r'^\*\*Status:\*\*')
T = re.compile(r'^\s*$|^\*\*[^*:]+:\*\*|^#{1,6}\s')
H = re.compile(r'^##\s')
n = {'wrapped_in': 0, 'wrapped_below': 0, 'plain_in': 0, 'plain_below': 0, 'none': 0}
no_h2 = 0
worst_block = (0, 0, '')
worst_field = (0, '')
for p in docs:
    L = open(p, encoding='utf-8', errors='replace').read().splitlines()
    lim = next((i for i, l in enumerate(L) if H.match(l)), None)
    if lim is None:
        no_h2 += 1
        lim = len(L)
    blk = sum(len(l) + 1 for l in L[:lim])
    if lim > worst_block[0]:
        worst_block = (lim, blk, p)
    i = next((i for i, l in enumerate(L) if S.match(l)), None)
    if i is None:
        n['none'] += 1
        continue
    j = i + 1
    while j < len(L) and not T.match(L[j]):
        j += 1
    if j - i > worst_field[0]:
        worst_field = (j - i, p)
    n[('wrapped_' if j - i > 1 else 'plain_') + ('below' if i >= lim else 'in')] += 1
print(f"docs={len(docs)}")
for k, v in n.items():
    print(f"  {k}={v}")
print(f"  wrapped_total={n['wrapped_in'] + n['wrapped_below']}"
      f" with_status={sum(v for k, v in n.items() if k != 'none')}")
print(f"docs_with_no_h2={no_h2}")
print(f"largest_header_block={worst_block[0]} lines, {worst_block[1]} bytes ({worst_block[2]})")
print(f"longest_status_extent={worst_field[0]} lines ({worst_field[1]})")
PY
```

Output on 2026-08-02:

```
docs=296
  wrapped_in=52
  wrapped_below=2
  plain_in=124
  plain_below=0
  none=118
  wrapped_total=54 with_status=178
docs_with_no_h2=0
largest_header_block=65 lines, 4492 bytes (docs/specs/ANTS-3579.md)
longest_status_extent=16 lines (docs/specs/ANTS-1397.md)
```

It is a **heredoc on purpose**: the same script written as `python3 -c "…"`
has its `$` and `\` rewritten by the shell before Python sees them, so a reader
who pastes it gets a regex that matches a literal `$` and silently different
counts.

The scan scope **includes this spec**, which is why the totals moved by one
between drafting and self-check; no figure the design rests on moved. Re-run
the command rather than trusting the transcript — a corpus count is true at a
date, and its date is above. § 6 makes this permanent.

**This population is wider than ANTS-3785's, and the counts differ for that
reason rather than by contradiction.** That spec measured `docs/specs/` and
reported 49 wrapped; this one measures everything `docs_index` indexes —
`docs/standards/`, `docs/decisions/`, `docs/journal/`, `docs/qa/` and the root
`*.md` files as well — and reports 54. § 6 makes the figure an output rather
than a transcription.

## 2. Surface

### 2.1 Buffer the header block, then call the shared helper

`SpecParse::headerField(const QStringList &lines, const QString &name)` needs a
line list; `scanDoc` streams. The change accumulates the header block — the
lines before the first `^## ` — into a bounded `QStringList`, then calls the
helper once. Everything else in the loop (headings, links, fence tracking, line
and byte counting) is untouched.

**`SpecParse` gains one exported predicate**, so the header-block bound is
shared rather than re-expressed. `headerField` already applies this rule
internally; a private `blockEndRx` in `docsindex.cpp` would be a second copy of
the very thing this spec exists to stop having copies of:

```cpp
// src/specparse.h — new, beside headerField.
bool isHeaderBlockEnd(const QString &line);   // true for ^##\s
```

`headerField`'s own internal `blockEndRe` is replaced by a call to it, so there
is one expression of the bound in the codebase and both callers use it.

```cpp
// src/docsindex.cpp — scanDoc locals, beside `QChar openFence;`
QStringList headerLines;
bool        headerDone = false;

// …inside the read loop, at the point the statusRx match used to sit
// (i.e. AFTER the over-long-line and fenced-block `continue`s — see below).
if (!headerDone) {
    if (SpecParse::isHeaderBlockEnd(line) ||
        headerLines.size() >= opts.maxHeaderBlockLines) {
        headerDone = true;                       // INV-3 bound / INV-4 cap
        r.status = SpecParse::headerField(headerLines, QStringLiteral("Status")).value;
        headerLines.clear();                     // released immediately (§ 4)
    } else {
        headerLines << line;
    }
}

// …after the read loop ends — the EOF flush. A document whose header block is
// never closed by a `^## ` (and never hits the cap) reaches EOF with lines
// still buffered; without this, its status would be silently lost (INV-8).
if (!headerDone) {
    r.status = SpecParse::headerField(headerLines, QStringLiteral("Status")).value;
    headerLines.clear();
}
```

`statusRx` is deleted, `#include "specparse.h"` added, and `docsindex` gains
`ants_core_lib`'s existing dependency on it — `SpecParse` is already in the
same library, so no build-graph change (§ 4).

**Three exits, not one, and the third is the one that is easy to miss.** The
block closes at the first `^## `, at the cap, or at EOF. Every document in the
scan scope has such a heading today (`docs_with_no_h2=0`, § 1), so the EOF path
is unreachable against the current corpus; it exists
because the bound must not depend on that staying true, and because a
`docs_index` run over a *different* project — which is the normal case, the
verb takes any `caller_cwd` — has no such guarantee at all.

**Where the buffer sits in the loop matters, and it is not free of
consequences.** The append happens where the `statusRx` match used to, which is
after `scanDoc`'s two `continue`s: the over-long-line guard
(`raw.size() > kMaxLineBytes`, INV-3 of ANTS-2139) and the fenced-block skip
(ANTS-3604). So a header-block line inside a fence, or longer than 1024 bytes,
never enters `headerLines`, where `headerField` called directly on the file's
raw lines would see it. That divergence is deliberate — it keeps every existing
`scanDoc` invariant intact — and it is why INV-2 states its equality with those
exclusions named rather than claiming an unconditional identity it cannot have.

### 2.2 The cap, and why it is not the byte budget

`Options` gains one field, in the shape ANTS-2139 already uses for every other
per-doc cap:

```cpp
constexpr int kMaxHeaderBlockLines = 256;   // INV-4 silent per-doc cap
struct Options {
    // … existing fields unchanged …
    int maxHeaderBlockLines = kMaxHeaderBlockLines;
};
```

256 is chosen against the corpus, not by feel: the largest header block in
scope is **65 lines / 4,492 bytes** (`docs/specs/ANTS-3579.md`, § 1's
`largest_header_block`) — so the cap
sits at roughly 4× the observed worst case and no current document reaches it.
Past the cap, buffering stops and the helper runs on what was collected — the
status is found if it was inside the cap and empty otherwise. That is
deliberately the same *silent* degradation ANTS-2139 INV-19 already specifies
for headings and links: no per-doc flag, verified through an `Options`
override.

**The existing `maxDocBytes` budget is not a substitute for this cap.** It
bounds the bytes *read*; the buffer's cost is the QStringList *held*, and a
`QString` line costs materially more than its UTF-8 bytes (the point ANTS-3636
records as "a line cap is not a size cap", in the other direction). A document
that opened with 4 MiB of header block would satisfy the byte budget and still
hold the whole thing.

**A line cap needs its own byte figure, for the same reason.** The two bounds
compose: `scanDoc` already skips any line over `kMaxLineBytes` (1024) before
the buffer sees it, so the capped worst case is
`256 × 1024 B = 256 KiB` of input, held as `QString` (UTF-16, plus per-`QString`
overhead) at roughly **512–600 KiB**. That is the number § 4 budgets against —
the corpus figure below it is what actually happens, not what is permitted.

### 2.3 What the emitted status changes to

The four classes are disjoint and sum to the 296 documents in scope:

| Class | Count | Today | After |
|---|---|---|---|
| Wrapped, inside the header block | 52 | first physical line only | whole joined value |
| Wrapped, **and** only below the first `## ` | 2 | that body line's first line | `""` |
| Unwrapped, inside the header block | 124 | unchanged | unchanged |
| No `**Status:**` line | 118 | `""` | `""` |

**The two below-block documents are also wrapped ones** — the 54 of § 1 is
`52 + 2`, not a fifth class alongside them. There is no
below-block-but-unwrapped row because that set is empty (measured: 0), and
saying so is the point: the same two documents appear in both figures, so
adding 54 and 2 would double-count them.

The second row is a **behaviour change, not a regression**: both documents
were reporting body prose as a document status, and ANTS-2139 INV-17 already
makes an empty status legitimate ("absence … is never an error"). It is called
out because INV-17's wording does not currently say *where* the line must
appear, which § 7 corrects.

## 3. Invariants

- **INV-1** — A `**Status:**` whose value wraps onto following lines yields the
  whole logical value, joined with single spaces, not its first physical line.
  *Test:* `docsindex_header_field` — a fixture doc whose status spans three
  lines → `entry.status` equals the joined string; the pre-fix code returns
  only the first line.
- **INV-2** — For a document whose header block contains **no fenced-code line,
  no line over `kMaxLineBytes`, and no more lines than `maxHeaderBlockLines`**,
  `docsindex` and `SpecParse::headerField` return the same value: within those
  bounds the rule has one implementation, not two that agree today. The three
  exclusions are `scanDoc`'s pre-existing skips and this spec's own cap (§ 2.1);
  outside them the inputs are deliberately different line lists, so an
  unconditional equality would be false rather than strict. *Test:* every
  fixture in `tests/features/docsindex_header_field/fixtures/` that satisfies
  the three conditions is fed to `DocsIndex::scanToEntry` and to
  `SpecParse::headerField` on the same file's lines, asserting equality per
  fixture; the suite also carries one fixture per exclusion, asserting the two
  **differ** there, so the bounds are pinned in both directions.
- **INV-3** — The search is bounded to the header block: a `**Status:**` line
  appearing only after the first `^## ` leaves `status` empty. *Test:* a
  fixture whose sole status line sits below a `## ` heading → `status == ""`.
  (Only the bound can reject this fixture: the field is well-formed and
  unwrapped, so INV-1's rule and the cap both accept it.)
- **INV-4** — The header buffer is capped at `maxHeaderBlockLines`; past the
  cap the status is whatever was found within it, and the cap is silent — no
  flag on the entry. *Test:* `Options{maxHeaderBlockLines:2}` over a doc whose
  `**Status:**` sits on line 5 → `status == ""`, entry still emitted with its
  headings.
- **INV-5** — ANTS-2139 INV-19 is unchanged: `maxDocBytes` still bounds the
  read, and a document whose budget expires before its header block closes is
  still emitted, with an empty status rather than a partial field value.
  *Test:* `Options{maxDocBytes:<small>}` over a doc whose status straddles the
  budget → entry exists, `status == ""`, no partial value.
- **INV-6** — A document with no `**Status:**` line is still indexed with
  `status == ""` and is never an error (ANTS-2139 INV-17's surviving half).
  *Test:* a fixture with no status line → indexed, `status == ""`.
- **INV-7** — `src/docsindex.cpp` adopts the shared rule rather than merely
  correcting its own: it calls `headerField(` and retains no field-matching
  regex of its own. *Test:* a source scrape in
  `tests/features/docsindex_header_field/`, with `CMakeLists.txt` gaining
  `SRC_DOCSINDEX_CPP_PATH` beside the `SRC_SPECPARSE_CPP_PATH` /
  `SRC_SPECLOG_CPP_PATH` definitions it already passes the bundle. **Both
  halves, and the absence half greps the identifier `statusRx`, never the
  `Status:` literal** — this is ANTS-3785 INV-6's rule applied to a third file,
  for its reason: a positive assertion alone passes a build that kept a
  hand-rolled matcher, and an absence assertion aimed at the literal would fail
  against a correct implementation that legitimately mentions `**Status:**` in
  a comment. The `#include` is not asserted: including a header does not prove
  the helper is called, and `headerField(` does.
- **INV-8** — A document with a `**Status:**` line and **no `^## ` heading
  anywhere** still yields that status: the header block is flushed at EOF, not
  abandoned. *Test:* a fixture with a header block, a status line and no `##`
  heading at all → `status` equals the field's value. (Only the EOF flush can
  satisfy this fixture: the `^## ` exit never fires and the block is far under
  the cap, so an implementation missing the flush returns `""` — which is
  exactly what the § 2.1 snippet did before this invariant existed.)

## 4. RAM / build cost

Peak heap gains one bounded `QStringList` per document *while that document's
header block is open*, released at whichever of the three exits fires — first
`^## `, cap, or EOF — and never held across documents (`headerLines.clear()`,
§ 2.1). Two figures, and the spec budgets against the first:

| | Lines | Held (QString) | vs `maxDocBytes` (4 MiB) |
|---|---|---|---|
| **Permitted** — at the cap | 256 | ~512–600 KiB (§ 2.2) | ~13% |
| **Observed** — corpus worst case | 65 | ~9 KiB (4,492 B of text) | ~0.2% |

The permitted figure is the one that bounds the feature; the observed figure is
what the current corpus costs. ANTS-2139 § 4's binding ceiling —
`kMaxCacheBytes` = 8 MiB over the whole index — is untouched, since nothing new
is stored in `DocEntry`; this buffer is transient per-document scan state, not
index content.

No new build target, no new library, no external dependency:
`SpecParse` and `DocsIndex` are both already in `ants_core_lib`, so the
`#include` adds a compile edge inside one library and no link edge.

## 5. Out of scope

- **Re-wrapping or repairing the 54 wrapped documents.** They are well-formed;
  the reader was wrong, not the documents.
- **The two below-block documents' prose.** `ROADMAP.md` and the journal entry
  keep their text; only the index stops mistaking it for a status.
- **`Kind` and the other header fields.** `docsindex` extracts `Status` alone
  (`ScanResult` has no `kind` member), and adding fields is a `docs_index`
  surface change, not this fix.
- **ANTS-1253's two-fields-on-one-line header** — ANTS-3787. It affects what
  `Kind` returns, which this spec does not read.
- **A streaming form of the rule.** Considered and rejected: a line-at-a-time
  accumulator inside `scanDoc` would hold only one field's lines (16 at the
  corpus worst case — `docs/specs/ANTS-1397.md`, § 1's
  `longest_status_extent`) instead of the block's 65, but it re-expresses the
  **field-extent terminator set** — blank line, next field marker, ATX
  heading — in a second place, which is the duplication INV-2 and INV-7 exist
  to end. The buffer's cost is small enough (§ 4) that buying it back at the
  price of a second implementation is a bad trade.

  **The block-end bound is a different matter and is not duplicated**, which is
  why § 2.1 exports `SpecParse::isHeaderBlockEnd` instead of writing a local
  `^##\s` regex: `docsindex` needs that predicate to know when to *flush*, and
  the same rejection argument would otherwise apply to the design this spec
  accepts. Should the cap ever bind in practice, the streaming form is the
  alternative to revisit — and the exported predicate is what it would build
  on.

## 6. Tests

Feature test: `tests/features/docsindex_header_field/`, paired with its own
`spec.md`, sourced into the same bundle that carries
`tests/features/mcp_docs_index/` (`CMakeLists.txt`, the ANTS-2139 entry) — a
new source line, not an `add_executable`. Covers **INV-1..INV-8**, behavioural
and source-scrape halves alike. Label `features;fast`. Each assertion is
verified to **fail against pre-fix `docsindex.cpp`** before the fix is restored,
per the project test convention.

**This directory is the single home for all eight**, including INV-7's scrape.
`tests/features/spec_field_extent/` is deliberately *not* extended: its scrape
asserts ANTS-3785 INV-6, whose text pins it to "**both** files: `speclog.cpp`
and `specparse.cpp`", so adding a third file there would silently rewrite
another spec's stated test surface. A third consumer gets a third scrape in its
own spec's own directory, and ANTS-3785 keeps the invariant it shipped with
(§ 7 records the annotation).

Fixtures live in `tests/features/docsindex_header_field/fixtures/` and are the
"fixture corpus" INV-2 names — hermetic files written for the test, **not** the
repo's 296 real documents. Driving invariants off the live corpus would make
the suite fail whenever someone edits an unrelated spec, and would re-import
§ 1's dated counts into a test that must not depend on them.

`tools/spec-header-survey.py` gains a `--scope=docs-index` mode that walks
`walkDocs`'s two roots instead of one directory and prints the cross-tabulation
§ 1 quotes, including `largest_header_block` and `longest_status_extent` — the
two figures § 2.2 and § 5 size their choices against. The § 1 figures then
become the tool's output rather than a transcription, which is the rung
`documentation.md` asks for and the reason § 1's command is recorded inline in
the meantime.

`tools/spec-header-survey.py` gains a `--scope=docs-index` mode that walks
`walkDocs`'s two roots instead of one directory and prints the wrapped and
below-header-block counts. The § 1 figures then become the tool's output
rather than a transcription of a one-off command, which is the rung
`documentation.md` asks for and the reason the § 1 command is recorded inline
in the meantime.

## 7. Cross-doc impact

- **`docs/specs/ANTS-2139.md` INV-17** — currently "a `**Status:**` line sets
  `status`", which is silent on position and is now false for the 2
  below-block documents. Amended in place to say the line must appear in the
  header block, annotated `INV-17 amended by ANTS-3786` per
  `specs.md` § 5.5. **Not renumbered.**
- **`docs/specs/ANTS-2139.md` INV-19** — gains `maxHeaderBlockLines` in its
  list of silent per-doc caps, same annotation form.
- **`docs/specs/ANTS-3785-header-field-extent.md` INV-6** — reads "the two
  consumers § 2.2 names share one implementation", with a test surface pinned
  to "**both** files". Both stay literally true and **its test is not touched**
  (§ 6); it gains an annotation naming `docsindex` as a third consumer carrying
  its own scrape, so a reader does not conclude the rule has exactly two.
- **`docs/specs/ANTS-3785-header-field-extent.md` § 2.1 / INV-10** — the
  header-block bound moves from a private `blockEndRe` inside `headerField` to
  the exported `SpecParse::isHeaderBlockEnd` (§ 2.1). Behaviour is identical
  and INV-10 still holds verbatim; the annotation records that the predicate is
  now shared, since a reader of that spec would otherwise expect the regex to
  be internal.
- **`ROADMAP.md`** — ANTS-3786 flips to 🚧 when implementation starts.
- **`CHANGELOG.md`** — a `Fixed` entry at release.
- **`CLAUDE.md`** — no change; `docs_index` is described by capability, not by
  its status-parsing rule.

## Cold-eyes loop log

| Loop | Date | Lanes | Findings | Resolution |
|---|---|---|---|---|
| 1 | 2026-08-02 | 2 cold `general-purpose` lanes, one byte-identical shared packet, loop log withheld via a scrubbed copy | C 1 · H 2 · M 5 · L 7 · I 1 — 16 verified, 0 dismissed | All 15 actionable fixed; the INFO carried to the report. **Both lanes independently returned the same CRITICAL**, which is the strongest evidence the run produced: the § 2.1 snippet assigned `status` only inside its flush branch, so a document with no `^## ` heading and a header block under the cap would reach EOF with the field buffered and unread — a silent regression against the code this spec replaces, in a draft whose own § 2.3 table asserted such documents were "unchanged". Fixed by a third exit (the EOF flush) and pinned by a new **INV-8**. Two HIGHs were contract defects the author could not see: INV-2 claimed an unconditional equality that `scanDoc`'s fence and over-long-line skips make false, and INV-7 was written in exactly the scrape shape ANTS-3785 INV-6 spells out as wrong (literal instead of identifier, `#include` instead of call-site) — a sibling invariant the author had read and mis-applied. One MEDIUM was the design arguing against itself: § 5 rejected the streaming alternative for duplicating the rule while § 2.1 duplicated the block-end bound, resolved by exporting `SpecParse::isHeaderBlockEnd`. The Phase 4b sweep then caught one defect the fixes themselves introduced — a rewritten § 2.1 cited `plain_below=0` as evidence every document carries a `^## `, which that field does not measure; the § 1 command now prints `docs_with_no_h2` and the byte figure § 2.2 needs. |
