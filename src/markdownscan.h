// ANTS-3603 — shared CommonMark fence primitives, hoisted from the verbatim
// copies that had accumulated in feedbackfile.cpp and speclog.cpp (Rule of
// Three: the second call-site was the trigger). Qt6::Core-only, lives in
// ants_core_lib so it is unit-testable without RemoteControl / MainWindow
// (mirrors readlog.h / feedbackfile.h / speclog.h).
//
// The single fence rule every markdown parser in the project shares:
// `^ {0,3}(```|~~~)` — a fence opener is a run of three backticks or three
// tildes with up to 3 leading spaces. The indent is space-only per CommonMark
// (a `\s` class would admit a tab/CR/FF and misread a body line as a fence,
// swallowing findings — ANTS-3598). A fence is closed only by a line opening
// with the SAME fence character.
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
QChar fenceOpenerChar(const QString &line);

// Per-line "inside a fenced code block" mask, one bool per input line. The
// opener and closer lines are themselves masked true (they are fence syntax,
// not prose). An unterminated fence masks true to end-of-input — the same
// CommonMark leniency the local scanners had. A closer must repeat the
// opener's character: a ``` block is not closed by a ~~~ line.
QVector<bool> fenceMask(const QStringList &lines);

}  // namespace MarkdownScan
