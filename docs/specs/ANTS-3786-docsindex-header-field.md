# ANTS-3786 — Read docsindex's Status field with the shared header-field rule

**Status:** spec draft (2026-08-02).
**Kind:** fix.
**Source:** ROADMAP.md ANTS-3786 (in-session-2026-08-02; found in ANTS-3785
cold-eyes loop 2, while verifying that spec's own "two consumers, no third"
claim — the claim was false because the search behind it was too narrow).
**Blocked by:** ANTS-3785 (shipped 2026-08-02; owns `SpecParse::headerField`,
the rule this adopts).
**Amends:** ANTS-2139 INV-17 (§ 7).

**Layman:** A third piece of code reads a document's status line and, like the
two already fixed, stops at the first line — so a status written across two
lines is cut short in the documentation index.

## 1. Problem

`src/docsindex.cpp::scanDoc()` carries its own `**Status:**` regex, matched
per line inside the streaming read loop, and keeps the first match's captured
tail. It is the ANTS-3672 defect verbatim: a status whose value wraps onto a
following line is truncated at its first physical line. ANTS-3672 fixed this
in `spec_query`, and ANTS-3785 generalised the rule into
`SpecParse::headerField()`; `docsindex` was scoped out of that change and named
here (ANTS-3785 § 5).

Three consequences, in the order they matter:

1. **Truncated status.** `docs_index` under-reports the status of **54** of
   the **178** in-scope documents that carry a `**Status:**` line. It feeds
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

All three figures come from one command, run against `docs_index`'s real scan
scope (`<root>/*.md` non-recursive, then `<root>/docs/**/*.md` recursive — the
walk `src/docsindex.cpp::walkDocs()` performs), on 2026-08-02:

```bash
python3 -c "
import glob,re
d=sorted(glob.glob('*.md'))+sorted(glob.glob('docs/**/*.md',recursive=True))
S=re.compile(r'^\*\*Status:\*\*');T=re.compile(r'^\s*\$|^\*\*[^*:]+:\*\*|^#{1,6}\s');H=re.compile(r'^##\s')
t=w=b=0
for p in d:
    L=open(p,encoding='utf-8',errors='replace').read().splitlines()
    lim=next((i for i,l in enumerate(L) if H.match(l)),len(L))
    i=next((i for i,l in enumerate(L) if S.match(l)),None)
    if i is None: continue
    t+=1
    j=i+1
    while j<len(L) and not T.match(L[j]): j+=1
    if j-i>1: w+=1
    if i>=lim: b+=1
print(f'docs={len(d)} with_status={t} wrapped={w} below_header_block={b}')"
# docs=296 with_status=178 wrapped=54 below_header_block=2
```

The scan scope **includes this spec**, which is why the totals moved by one
between drafting and self-check; `wrapped` and `below_header_block` are the
figures the design rests on and neither moved. Re-run the command rather than
trusting the comment — a corpus count is true at a date, and its date is above.

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

```cpp
// src/docsindex.cpp, inside scanDoc's read loop — replaces the statusRx match.
if (!headerDone) {
    if (blockEndRx.match(line).hasMatch() ||
        headerLines.size() >= opts.maxHeaderBlockLines) {
        headerDone = true;                       // INV-3 bound / INV-4 cap
        r.status = SpecParse::headerField(headerLines, QStringLiteral("Status")).value;
        headerLines.clear();                     // released immediately (§ 4)
    } else {
        headerLines << line;
    }
}
```

`statusRx` is deleted, `#include "specparse.h"` added, and `docsindex` gains
`ants_core_lib`'s existing dependency on it — `SpecParse` is already in the
same library, so no build-graph change (§ 4).

The header block is closed and the helper called at the **first `^## `**, not
at EOF, so a document is never buffered whole. Every document in the scan scope
has such a heading today (measured 2026-08-02: `docs with NO '## ' heading at
all: 0`), which is why the cap in § 2.2 exists — the bound must not depend on
that staying true.

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
scope is **65 lines / 4,492 bytes** (`docs/specs/ANTS-3579.md`) — the same
2026-08-02 walk as § 1, taking each document's lines before its first `^## `
and keeping the maximum — so the cap
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
- **INV-2** — `docsindex` and `SpecParse::headerField` return the **same**
  value for the same document: the rule has one implementation, not two that
  agree today. *Test:* the fixture corpus is fed to `DocsIndex::scanToEntry`
  and to `SpecParse::headerField` directly, asserting equality per document —
  a divergence fails even when both are individually plausible.
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
- **INV-7** — `src/docsindex.cpp` carries no `**Status:**` pattern of its own;
  the third copy is gone rather than merely corrected. *Test:* the existing
  ANTS-3785 INV-6 source scrape, extended — `CMakeLists.txt` gains
  `SRC_DOCSINDEX_CPP_PATH` beside the `SRC_SPECPARSE_CPP_PATH` /
  `SRC_SPECLOG_CPP_PATH` definitions it already passes the bundle, and the
  test asserts the file holds no `Status:` regex literal and does include
  `specparse.h`. A build that deletes the regex without adopting the helper
  fails, which a behavioural test alone cannot catch.

## 4. RAM / build cost

Peak heap gains one bounded `QStringList` per document *while that document's
header block is open*, released at the first `^## ` and never held across
documents (`headerLines.clear()`, § 2.1). Worst case at the cap is 256 lines;
against the corpus it is 65 lines / 4,492 bytes, roughly 0.1% of the 4 MiB
`maxDocBytes` already permitted per document. ANTS-2139 § 4's binding
ceiling — `kMaxCacheBytes` = 8 MiB over the whole index — is untouched, since
nothing new is stored in `DocEntry`.

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
  corpus worst case — `docs/specs/ANTS-1397.md`, longest `**Status:**` extent
  in the same walk) instead of the block's 65, but it re-expresses the
  terminator logic in a second place and so re-creates the duplication INV-2
  and INV-7 exist to end. The buffer's cost is small enough (§ 4) that buying
  it back at the price of a second implementation is a bad trade. Should the
  cap ever bind in practice, this is the alternative to revisit.

## 6. Tests

Feature test: `tests/features/docsindex_header_field/`, paired with its own
`spec.md`, sourced into the same bundle that carries
`tests/features/mcp_docs_index/` (`CMakeLists.txt`, the ANTS-2139 entry) — a
new source line, not an `add_executable`. Covers INV-1..INV-7. Label
`features;fast`. Each assertion is verified to **fail against pre-fix
`docsindex.cpp`** before the fix is restored, per the project test convention.

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
  consumers § 2.2 names share one implementation". It stays literally true and
  gains an annotation naming `docsindex` as the third, so a reader does not
  conclude the rule has exactly two consumers.
- **`ROADMAP.md`** — ANTS-3786 flips to 🚧 when implementation starts.
- **`CHANGELOG.md`** — a `Fixed` entry at release.
- **`CLAUDE.md`** — no change; `docs_index` is described by capability, not by
  its status-parsing rule.

## Cold-eyes loop log

No loop has run. The table below is filled by `/cold-eyes`, one row per loop
as it closes.

| Loop | Date | Lanes | Findings | Resolution |
|---|---|---|---|---|
