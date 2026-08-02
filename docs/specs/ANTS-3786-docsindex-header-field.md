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
   **177** in-scope documents it reads one from: **52** truncated at their
   first physical line, and **2** that are truncated *and* taken from body
   prose rather than the header block (the same two, counted once — see
   consequence 3, which is why they are wrong twice over). It feeds
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

**Every figure this spec quotes comes from the command below**, which
**simulates both code paths** rather than counting `**Status:**` lines. That
distinction is load-bearing: a plain line-count answers a question the spec is
not asking, because `scanDoc` does not look at every line. It skips any line
over `kMaxLineBytes` (ANTS-2139 INV-3) and every line inside a fenced block
(ANTS-3604) *before* its matcher runs, and its matcher requires a non-empty
same-line tail (`(.+)$`) where `headerField` accepts an empty one (`(.*)$`).
A count that models none of these reports a status for documents the scanner
never reads one from — measured: exactly one, `docs/specs/ANTS-2161.md`, whose
`**Status:**` line is a single run well over 1024 bytes and is therefore
invisible to `docs_index` today **and** after this change.

The scope is `walkDocs`'s: `<root>/*.md` non-recursive, then
`<root>/docs/**/*.md` recursive. Two `walkDocs` behaviours the script does not
model, checked by hand on 2026-08-02 and both inert here: a
`.ants/project.json` `docs_dir` override (this repo ships no such file) and
`QDir::NoSymLinks` (no `.md` in scope is a symlink). On a project where either
holds, re-derive before trusting these counts.

Run on 2026-08-02:

```bash
python3 - <<'PY'
import glob, re
MAXLINE = 1024
FENCE = re.compile(r'^ {0,3}(```+(?!.*`)|~~~+)')
OLD  = re.compile(r'^\*\*Status:\*\*\s*(.+)$')   # scanDoc today: non-empty tail
NEW  = re.compile(r'^\*\*Status:\*\*\s*(.*)$')   # headerField: empty tail ok
TERM = re.compile(r'^\s*$|^\*\*[^*:]+:\*\*|^#{1,6}\s')
H2   = re.compile(r'^##\s')

def considered(lines):                 # the lines scanDoc actually looks at
    out, opener = [], None
    for line in lines:
        if len(line.encode('utf-8')) + 1 > MAXLINE:
            continue                   # over-long-line skip (ANTS-2139 INV-3)
        m = FENCE.match(line)
        if opener is None and m:
            opener = m.group(1)[0]; continue
        if opener is not None:
            if m and m.group(1)[0] == opener:
                opener = None
            continue                   # fenced-block skip (ANTS-3604)
        out.append(line)
    return out

def today(seen):
    for line in seen:
        m = OLD.match(line)
        if m:
            return m.group(1).strip()
    return ""

def after(seen):                       # buffer to first ^##, then headerField
    buf = []
    for line in seen:
        if H2.match(line):
            break
        buf.append(line)
    for i, line in enumerate(buf):
        m = NEW.match(line)
        if not m:
            continue
        parts, j = [m.group(1).strip()], i + 1
        while j < len(buf) and not TERM.match(buf[j]):
            parts.append(buf[j].strip()); j += 1
        return " ".join(p for p in parts if p)
    return ""

docs = sorted(glob.glob('*.md')) + sorted(glob.glob('docs/**/*.md', recursive=True))
cls = {}
for p in docs:
    seen = considered(open(p, encoding='utf-8', errors='replace').read().splitlines())
    t, a = today(seen), after(seen)
    k = ('both_empty' if t == a == "" else 'unchanged_value' if t == a
         else 'truncated_now_whole' if t and a.startswith(t)
         else 'body_prose_now_empty' if t and not a else 'other')
    cls[k] = cls.get(k, 0) + 1
print(f"docs={len(docs)}")
for k in ('truncated_now_whole', 'body_prose_now_empty', 'unchanged_value',
          'both_empty', 'other'):
    print(f"  {k}={cls.get(k, 0)}")
PY
```

Output on 2026-08-02:

```
docs=296
  truncated_now_whole=52
  body_prose_now_empty=2
  unchanged_value=123
  both_empty=119
  other=0
```

**`other=0` is the part that makes this evidence rather than a tally.** It is
the bucket for a document the change affects in a way the design did not
predict; an empty one says every one of the 296 lands in a class § 2.3
describes. A non-zero `other` would be a design finding, not a counting error.

It is a **heredoc on purpose**: written as `python3 -c "…"` the shell rewrites
`$` and `\` before Python sees them, so a pasted copy silently measures
something else.

The scope **includes this spec**. Re-run the command rather than trusting the
transcript — a corpus count is true at a date, and its date is above. § 6 makes
it permanent.

**This population is wider than ANTS-3785's, and the counts differ for that
reason rather than by contradiction.** That spec measured `docs/specs/` and
reported 49 wrapped; this one measures everything `docs_index` indexes —
`docs/standards/`, `docs/decisions/`, `docs/journal/`, `docs/qa/` and the root
`*.md` files as well — and reports 54.

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
bool        budgetHit  = false;   // the maxDocBytes break, distinguished from EOF

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
// It does NOT run when the loop ended on the byte budget: that buffer is a
// truncated prefix of the header block, not a complete one (INV-5).
if (!headerDone && !budgetHit) {
    r.status = SpecParse::headerField(headerLines, QStringLiteral("Status")).value;
}
headerLines.clear();
```

The budget check gains the flag it needs, one line at the existing `break`:

```cpp
if (budget > opts.maxDocBytes) { budgetHit = true; break; }   // was: break;
```

`statusRx` is deleted, `#include "specparse.h"` added, and `docsindex` gains
`ants_core_lib`'s existing dependency on it — `SpecParse` is already in the
same library, so no build-graph change (§ 4).

**Four ways out of the header block, and only three of them produce a status.**
It closes at the first `^## `, at the cap, at EOF — and the read loop can also
end on the `maxDocBytes` budget, which is *not* a close: the buffer at that
point is a truncated prefix, so the flush is suppressed and the status stays
empty (INV-5). Conflating the budget break with EOF is the specific mistake
this paragraph exists to prevent, because both leave the loop with
`headerDone == false` and only one of them means "the header block ended".

Every document in the scan scope carries a `^## ` today, so the EOF path is
unreachable against the current corpus; it exists
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

These are the simulator's four classes (§ 1), disjoint by construction and
summing to the 296 documents in scope:

| Class | Count | Today | After |
|---|---|---|---|
| `truncated_now_whole` — wrapped, inside the header block | 52 | first physical line only | whole joined value |
| `body_prose_now_empty` — wrapped, **and** only below the first `## ` | 2 | that body line's first line | `""` |
| `unchanged_value` — read identically by both paths | 123 | unchanged | unchanged |
| `both_empty` — no status either way | 119 | `""` | `""` |

Two things the row labels alone would hide. **The 2 below-block documents are
also wrapped ones**, so § 1's 54 is `52 + 2` — the *classes* are disjoint, the
two *figures* overlap, and adding 54 and 2 would count those documents twice.
And `both_empty`'s 119 is 118 documents with no `**Status:**` line plus
**one that has one nobody can read**: `docs/specs/ANTS-2161.md`, whose status
line exceeds `kMaxLineBytes` and is skipped before either matcher sees it. This
change does not fix that document and does not try to — § 5.

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
  unconditional equality would be false rather than strict. *Test:* each
  fixture in `tests/features/docsindex_header_field/fixtures/` satisfying the
  three conditions is indexed with `DocsIndex::build()` over a `QTemporaryDir`
  and its `DocEntry::status` compared to `SpecParse::headerField` called on the
  same file's lines; the suite also carries one fixture per exclusion,
  asserting the two **differ** there, so the bounds are pinned in both
  directions.
- **INV-3** — The search is bounded to the header block: a `**Status:**` line
  appearing only after the first `^## ` leaves `status` empty. *Test:* a
  fixture whose sole status line sits below a `## ` heading → `status == ""`.
  (Only the bound can reject this fixture: the field is well-formed and
  unwrapped, so INV-1's rule and the cap both accept it.)
- **INV-4** — The header buffer is capped at `maxHeaderBlockLines`; past the
  cap the status is whatever was found within it, and the cap is silent — no
  flag on the entry. *Test:* `DocsIndex::build()` with
  `Options{maxHeaderBlockLines:2}` over a doc whose `**Status:**` sits on
  line 5 → `status == ""`, entry still emitted with its headings.
- **INV-5** — ANTS-2139 INV-19 is unchanged: `maxDocBytes` still bounds the
  read, and a document whose budget expires before its header block closes is
  still emitted, with an empty status rather than a partial field value. The
  EOF flush is suppressed on that path (§ 2.1's `budgetHit`) precisely so a
  truncated buffer cannot be mistaken for a complete header block. *Test:*
  `Options{maxDocBytes:<small>}` over a doc whose `**Status:**` value straddles
  the budget → entry exists, `status == ""`. (Only the suppression can satisfy
  this: the field's opening line is inside the budget, so an unguarded flush
  would return its first line — a partial value, and a visibly different
  assertion from the empty string.)
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
  the helper is called, and `headerField(` does. The absence half also covers
  the **block-end** pattern (`^##` as a `QRegularExpression` in
  `docsindex.cpp`), so an implementation that adopts `headerField` but
  hand-rolls the bound instead of calling `isHeaderBlockEnd` fails too — that
  bound is the second rule this change de-duplicates (§ 2.1), and without this
  clause nothing tests it.
- **INV-8** — A document with a `**Status:**` line and **no `^## ` heading
  anywhere** still yields that status: the header block is flushed at EOF, not
  abandoned. *Test:* a fixture with a header block, a status line and no `##`
  heading at all → `status` equals the field's value. (Only the EOF flush can
  satisfy this fixture: the `^## ` exit never fires and the block is far under
  the cap, so an implementation without the flush returns `""`.)
- **INV-9** — The corpus figures are reproducible from the shipped tree:
  `tools/spec-header-survey.py --scope=docs-index` prints the four simulator
  classes § 1 quotes, over `walkDocs`'s scope. *Test:* the feature test runs
  the tool against a fixture tree with one document of each class and asserts
  the four printed counts — so a figure in this spec can be re-derived rather
  than trusted, and a change to `scanDoc` that moves the classes breaks the
  tool's test rather than silently ageing the spec.

## 4. RAM / build cost

Peak heap gains one bounded `QStringList` per document *while that document's
header block is open*, released at whichever of the three exits fires — first
`^## `, cap, or EOF — and never held across documents (`headerLines.clear()`,
§ 2.1). Two figures, and the spec budgets against the first:

| | Lines | Held (QString) |
|---|---|---|
| **Permitted** — at the cap | 256 | ~512–600 KiB (§ 2.2) |
| **Observed** — corpus worst case | 65 | ~9 KiB (4,492 B of text) |

The permitted figure is the one that bounds the feature; the observed figure is
what the current corpus costs. Neither is compared against `maxDocBytes`: that
is a **read** budget governing bytes streamed off disk, not a heap ceiling, so
a percentage of it would look like a headroom claim while measuring nothing.
The heap ceiling that does bind is ANTS-2139 § 4's `kMaxCacheBytes` = 8 MiB
over the whole index, and it is untouched: nothing new is stored in `DocEntry`,
since this buffer is transient per-document scan state rather than index
content.

No new build target, no new library, no external dependency, and **no new link
edge**: `SpecParse` and `DocsIndex` are both already in `ants_core_lib`, so the
`#include` adds a compile edge inside one library. `CMakeLists.txt` is still
edited twice — a `SRC_DOCSINDEX_CPP_PATH` definition for INV-7's scrape and a
bundle source line for the new test (§ 6) — which is build-file churn without
being a build-graph change.

## 5. Out of scope

- **Re-wrapping or repairing the 52 wrapped documents.** They are well-formed;
  the reader was wrong, not the documents.
- **`docs/specs/ANTS-2161.md`'s unreadable status.** Its `**Status:**` line is
  one run over `kMaxLineBytes`, so ANTS-2139 INV-3's over-long-line guard drops
  it before any matcher — today and after this change alike (§ 1). Fixing it
  means either raising that guard or re-wrapping the document, and both are
  ANTS-2139's call, not this spec's. Named rather than left silent because it
  is the one document whose status this change might be expected to fix and
  does not.
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
new source line, not an `add_executable`. Covers **INV-1..INV-9** — behavioural,
source-scrape and tool halves alike. Label `features;fast`. Each assertion is
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

`tools/spec-header-survey.py` gains a `--scope=docs-index` mode (INV-9) that
walks `walkDocs`'s two roots instead of one directory and prints the four
simulator classes § 1 quotes, plus `largest_header_block` and
`longest_status_extent` — the two figures § 2.2 and § 5 size their choices
against. The § 1 figures then become the tool's output rather than a
transcription, which is the rung `documentation.md` asks for and the reason
§ 1's command is recorded inline in the meantime.

## 7. Cross-doc impact

- **`docs/specs/ANTS-2139.md` INV-17** — currently "a `**Status:**` line sets
  `status`", which is silent on position and is now false for the 2
  below-block documents. Amended in place to say the line must appear in the
  header block, annotated `INV-17 amended by ANTS-3786` per
  `specs.md` § 5.5. **Not renumbered.**
- **`docs/specs/ANTS-2139.md` INV-19** — gains `maxHeaderBlockLines` in its
  list of silent per-doc caps, same annotation form.
- **`docs/specs/ANTS-2139.md` § 2.3's `constexpr` / `struct Options` block** —
  enumerates every cap and every `Options` field, so it gains
  `kMaxHeaderBlockLines` and `maxHeaderBlockLines` alongside the INV-19 prose.
  Listed separately because amending the invariant text and leaving the code
  block short is the likelier half-fix, and the block is what an implementer
  copies from.
- **`docs/specs/ANTS-3785-header-field-extent.md` INV-6** — reads "the two
  consumers § 2.2 names share one implementation", with a test surface pinned
  to "**both** files". Both stay literally true and **its test is not touched**
  (§ 6); it gains an annotation naming `docsindex` as a third consumer carrying
  its own scrape, so a reader does not conclude the rule has exactly two.
  **One clause inside it does go false and must be corrected in the same
  edit**: INV-6 currently reads "`docsindex.cpp` holds a third copy that this
  change does not fold in (§ 5, ANTS-3786)", which stops being true the day
  this ships. The same is true of ANTS-3785 § 5's out-of-scope bullet naming
  this item. Both are superseded rather than annotated — a stale "does not fold
  in" is worse than silence, because it tells a reader the duplication is still
  there.
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
| 2 | 2026-08-02 | 2 cold `general-purpose` lanes, same byte-identical packet, no prior-loop context | C 1 · H 4 · M 6 · L 7 · I 1 — 19 verified, 0 dismissed, **2 of them found during verification rather than by a lane** | **No loop-1 finding resurfaced, which is the evidence those fixes held.** All 18 actionable fixed. **The CRITICAL was collateral from loop 1's own fix**: the EOF flush added there ran on the `maxDocBytes` `break` too, and that buffer is a truncated prefix — so the flush would emit exactly the partial value INV-5 forbids, making the two unbuildable together. Fixed with a `budgetHit` guard, and the "three exits" passage that caused it became four, since the budget break and EOF both leave the loop with `headerDone == false` and only one means the block ended. **The largest item was not a lane's**: verifying lane B's HIGH about the measurement script — that it modelled neither `scanDoc`'s over-long-line skip, its fence skip, nor its non-empty-tail matcher — meant replacing the count with a **simulation of both code paths**, which moved two figures (`unchanged` 124 → 123, `both_empty` 118 → 119) and surfaced `docs/specs/ANTS-2161.md`, whose `**Status:**` line exceeds `kMaxLineBytes` and is therefore unreadable before and after this change; it is now named in § 5. The second verification-found item killed a fictional test seam: INV-2 and INV-4 drove tests through `DocsIndex::scanToEntry`, which `docsindex.h` does not export — both now go through `build()`, the seam the sibling suite actually uses. |
| 1 | 2026-08-02 | 2 cold `general-purpose` lanes, one byte-identical shared packet, loop log withheld via a scrubbed copy | C 1 · H 2 · M 5 · L 7 · I 1 — 16 verified, 0 dismissed | All 15 actionable fixed; the INFO carried to the report. **Both lanes independently returned the same CRITICAL**, which is the strongest evidence the run produced: the § 2.1 snippet assigned `status` only inside its flush branch, so a document with no `^## ` heading and a header block under the cap would reach EOF with the field buffered and unread — a silent regression against the code this spec replaces, in a draft whose own § 2.3 table asserted such documents were "unchanged". Fixed by a third exit (the EOF flush) and pinned by a new **INV-8**. Two HIGHs were contract defects the author could not see: INV-2 claimed an unconditional equality that `scanDoc`'s fence and over-long-line skips make false, and INV-7 was written in exactly the scrape shape ANTS-3785 INV-6 spells out as wrong (literal instead of identifier, `#include` instead of call-site) — a sibling invariant the author had read and mis-applied. One MEDIUM was the design arguing against itself: § 5 rejected the streaming alternative for duplicating the rule while § 2.1 duplicated the block-end bound, resolved by exporting `SpecParse::isHeaderBlockEnd`. The Phase 4b sweep then caught one defect the fixes themselves introduced — a rewritten § 2.1 cited `plain_below=0` as evidence every document carries a `^## `, which that field does not measure; the § 1 command now prints `docs_with_no_h2` and the byte figure § 2.2 needs. |
