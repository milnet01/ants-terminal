// ANTS-3603 — shared CommonMark fence primitives, hoisted from the verbatim
// copies that had accumulated in feedbackfile.cpp and speclog.cpp (Rule of
// Three: the second call-site was the trigger). Qt6::Core-only, lives in
// ants_core_lib so it is unit-testable without RemoteControl / MainWindow
// (mirrors readlog.h / feedbackfile.h / speclog.h).
//
// The single fence rule every markdown parser in the project shares:
// `^ {0,3}(```+(?!.*`)|~~~+)` — a fence opener is a run of three or more
// backticks or tildes with up to 3 leading spaces. The indent is space-only per
// CommonMark (a `\s` class would admit a tab/CR/FF and misread a body line as a
// fence, swallowing findings — ANTS-3598), and a BACKTICK fence's info string
// may hold no backtick (CommonMark § 4.5), so a multi-backtick inline span is a
// paragraph and not an opener (ANTS-3655). A fence is closed only by a line
// opening with the SAME fence character.
//
// Consumers: feedbackfile.cpp (scanBoundaries), speclog.cpp (findSectionHeading),
// featurecoverage.cpp (ANTS-3600 doc-literal scan), docsindex.cpp (ANTS-3604
// fence-aware link scan), docintegrity.cpp (ANTS-3601), doccitations.cpp
// (ANTS-3653).
//
// ANTS-3649 also hoisted the inline-code-span scanner here from
// docintegrity.cpp, where it was an anonymous-namespace static: two verbs now
// need the span boundaries and they must not diverge.

#pragma once

#include <QChar>
#include <QString>
#include <QStringList>
#include <QVector>

class QRegularExpression;

namespace MarkdownScan {

// The shared fence-opener regex: `^ {0,3}(```|~~~)`. Capture group 1 is the
// fence run, so the opener character can be recovered to match the closer.
const QRegularExpression &fenceRe();

// The fence character (` or ~) that a fence opened on `line` would close on,
// or a null QChar() when `line` is not a fence opener.
//
// ANTS-3638 — `maxIndent` is the largest leading-space count that still opens
// a fence. It defaults to 3, which is the top-level CommonMark rule and makes
// this identical to matching fenceRe(). Callers that track list containers
// raise it, because CommonMark re-bases a list item's content at the marker's
// content column: a fence inside an item may be indented up to 3 spaces past
// THAT column. The indent is space-only either way (a tab never opens a
// fence — ANTS-3598).
// ANTS-3678 — `runLength`, when given, receives the length of the fence
// character's run. CommonMark § 4.5 requires a closing fence to be AT LEAST
// AS LONG as its opener, and the character alone cannot express that, so a
// caller pairing an opener with a closer needs both. Defaulted: the
// stateless callers below ask only "does a fence start here".
QChar fenceOpenerChar(const QString &line, int maxIndent = 3,
                      int *runLength = nullptr);

// ANTS-4820 — does `line` CLOSE a fence opened with `openChar` at run length
// `openRun`? CommonMark § 4.5: the same character, in a run at least as long.
//
// The rule lives here because it was written wrong at every site that had its
// own copy — each compared the fence CHARACTER alone, so a short run closed a
// longer block and the sample text after it leaked out. A predicate the sites
// share cannot drift the way a dozen hand-written comparisons did.
//
// Returns false for a null `openChar`, so a caller may ask without first
// checking whether it is inside a fence.
bool fenceCloses(const QString &line, QChar openChar, int openRun,
                 int maxIndent = 3);

// Per-line "inside a fenced code block" mask, one bool per input line. The
// opener and closer lines are themselves masked true (they are fence syntax,
// not prose). An unterminated fence masks true to end-of-input — the same
// CommonMark leniency the local scanners had. A closer must repeat the
// opener's character: a ``` block is not closed by a ~~~ line.
//
// ANTS-3638 — this is the one consumer that tracks list containers, so a
// fence nested in a list item opens correctly (a 4-space-indented ``` under
// a `- ` bullet used to be invisible, and its whole body was scanned as
// prose — docs/specs/ANTS-1238.md:319 harvested a bogus broken_link). The
// stateless `fenceOpenerChar` callers (feedbackfile, speclog, docsindex)
// keep the top-level rule: container tracking needs state they do not carry,
// and the files they scan fence at top level.
QVector<bool> fenceMask(const QStringList &lines);

// ANTS-3649 — the same mask, plus the 1-based line of an unclosed fence
// opener in `*unterminatedOpenerLine` (-1 when every fence closes; nullptr is
// accepted and makes this identical to the overload above).
//
// The fact is NOT recoverable from the mask, which is why this is an overload
// rather than post-processing: a document whose last line is a fence *closer*
// and one whose last line sits inside an *unclosed* fence both end in a run of
// `true`. Any rule inferred from the mask alone ("a trailing run of true means
// unterminated") false-alarms on every doc that ends in a properly closed code
// block — most specs in this repo. Only the scanner knows.
QVector<bool> fenceMask(const QStringList &lines, int *unterminatedOpenerLine);

// ANTS-3649 — one inline code span's CONTENT bounds. `startCol`/`endCol` are
// half-open [start, end) column indices excluding the delimiters, and
// `delimLen` is the backtick-run length, so the opening run starts at
// `startCol - delimLen` and the closing run ends at `endCol + delimLen`. A
// content-only struct cannot express those columns for a multi-backtick span,
// and consumers that measure a window from the delimiter need them.
struct CodeSpan {
    int startLine;   // 0-based
    int startCol;    // 0-based, first content column
    int endLine;     // 0-based
    int endCol;      // 0-based, one past the last content column
    int delimLen;    // backtick-run length of both delimiters
};

// Every inline code span in the document, in document order — hoisted out of
// `DocIntegrity::maskInlineCode`, which is still the only masking consumer but
// is no longer the only one that needs the boundaries.
//
// Whole-document, not line by line: a CommonMark span may cross a newline
// (§ 6.1), and a per-line pass leaves such a span's tail exposed — the defect
// ANTS-3635(a) fixed, where docs/specs/ANTS-1150.md:197-198 wraps a C++ lambda
// whose second line reads `[this](int idx)`.
//
// A span's closing run is searched FORWARD across lines but never past a blank
// line or a fence line — an inline span crosses neither. That rule decides
// where a span *ends*, so a genuinely boundary-free scan would change what the
// consumers count. A run with no equal-length partner is literal text per
// CommonMark and yields no span, so one stray backtick cannot swallow the rest
// of the document. Lines masked by `fence` are skipped entirely.
//
// Content is returned VERBATIM: CommonMark's one-space strip is the CALLER's
// job, so a caller matching an identifier is unaffected by it while a caller
// testing "fills the span" applies it.
QVector<CodeSpan> codeSpans(const QStringList &lines, const QVector<bool> &fence);

// ANTS-3659 — per-line "inside a doc-examples region" mask, one bool per input
// line, 0-based like fenceMask. A region is opened by a line matching
//     ^ {0,3}<!--[ \t]*doc-examples:[ \t]*(begin|end)[ \t]*-->[ \t]*$
// and closed by its `end` counterpart. Every line of a recognised region is
// masked, its two delimiters included (they are region syntax, not prose).
//
// Regions do not nest: a `begin` inside an open region is ignored and the
// region keeps its FIRST opener's line, which is what an unterminated doubled
// `begin` reports. An `end` with no open region is ignored and its line is not
// masked. An unterminated region masks true to the end of the INPUT — which for
// a truncated scan is the prefix, not the document — and reports its opener in
// *unterminatedOpenerLine as a 1-BASED line (-1 when balanced; nullptr
// accepted), matching fenceMask's overload.
//
// `fence` is required, not optional: a marker inside a fenced block is sample
// text, and docs/specs/ANTS-3659.md is a document that shows the marker inside
// a fence. It must be the mask for THESE lines; a size mismatch is a
// programming error, and in release a short mask reads as unfenced.
//
// A marker inside a multi-line INLINE code span is still a marker: consuming
// codeSpans here would make the lighter primitive depend on the heavier one for
// a document shape with no instance in this corpus.
//
// Lines must arrive with any trailing \r removed — the trailing [ \t]*$ matches
// neither \r nor \n, so a CRLF line would silently never open a region. The
// primitive cannot detect a violation it was handed; see ANTS-3659 § 2.1.
QVector<bool> exampleMask(const QStringList &lines, const QVector<bool> &fence,
                          int *unterminatedOpenerLine = nullptr);

// ANTS-3740 — ATX heading level: the leading '#' run when it is 1-6 long and
// followed by a space or end-of-line, else 0. Pass the TRIMMED line.
int headingLevel(const QString &trimmedLine);

// ANTS-3740 — the heading slug `read_region section=` resolves against:
// lowercase, every run of non-alphanumeric characters collapses to a single
// '-', leading/trailing '-' trimmed. Idempotent (the slug of a slug is
// itself), so a caller may pass either the heading text ("4.2 Emission model")
// or its slug ("4-2-emission-model").
//
// NOT DocIntegrity::gfmSlug, which implements GitHub's *anchor* rules
// (apostrophes stripped, underscores kept) and therefore disagrees on real
// headings in this corpus: `compact_resolved` slugs to `compact_resolved`
// there and `compact-resolved` here. The two are answers to different
// questions — a GitHub anchor URL vs. the key read_region matches — and a
// caller wanting a section it can then FETCH needs this one.
QString headingSlug(const QString &text);

// ANTS-3740 — every ATX heading in the document, in document order, with the
// body span each one owns. Hoisted out of ReadRegion::resolveSection when the
// cold-eyes brief became the second consumer: the brief publishes slugs a
// reviewer then passes back to `read_region section=`, so the two must share
// one transform or the index names sections that verb refuses.
//
// `endLine` is the last line the section owns — the line before the next
// heading at the same or a higher level (≤ '#' count), or lines.size() for the
// final section. Both `line` and `endLine` are 1-BASED, matching read_region's
// own line arithmetic; the input QStringList is 0-based as everywhere else
// here.
//
// Fence-aware via fenceMask, so a '#' inside a code block is never a heading
// (the ANTS-3674 defect: a document that *teaches* fenced code lost every
// heading after the first fence). `text` is the heading text with the leading
// '#' run and surrounding whitespace stripped, verbatim otherwise.
//
// ANTS-4520 — a heading inside a BLOCKQUOTE is a heading. `quoteDepth` is how
// many `>` markers it sits behind (0 for an ordinary one), and it is what
// bounds the section: a heading terminates an earlier one when it is less
// deeply quoted, or equally quoted at the same-or-higher level. Terminating on
// level alone would let a `> ## Previously` block swallow the plain `##`
// section after it; terminating only within one depth would run it to EOF.
struct Heading {
    int     line = 0;      // 1-based heading line
    int     level = 0;     // 1..6 = '#' count
    QString text;          // heading text, trimmed
    QString slug;          // headingSlug(text)
    int     endLine = 0;   // 1-based last line of this section's body
    int     quoteDepth = 0;  // '>' markers the heading sits behind
};
QVector<Heading> headings(const QStringList &lines);

// ANTS-4520 — strip a leading blockquote prefix (`>` plus one optional space,
// repeated, each marker admitting up to 3 leading spaces per CommonMark) and
// report how many markers were removed. Returns the line unchanged with
// *depth == 0 when there is no prefix. Shared because read_region's section
// resolver and file_outline's md mode must not disagree about what a heading
// is — the same reason ANTS-3740 hoisted headingSlug.
QString stripBlockquote(const QString &trimmedLine, int *depth = nullptr);

}  // namespace MarkdownScan
