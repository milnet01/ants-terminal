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
// fence-aware link scan), docintegrity.cpp (ANTS-3601).

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
QChar fenceOpenerChar(const QString &line, int maxIndent = 3);

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

}  // namespace MarkdownScan
