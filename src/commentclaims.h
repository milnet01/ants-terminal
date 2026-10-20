#pragma once

// comment_claims (ANTS-5786) — code comments whose claims can be checked:
// a quotation its file does not contain, a "see X" naming nothing, a ticket
// id the roadmap lacks (findings, tool-decided), and census numbers that go
// stale (candidates, for a reader). Contract:
// tests/features/mcp_comment_claims/spec.md. Its own translation unit so a
// test links it without RemoteControl (docs/standards/mcp-tools.md § Tests).

#include <QJsonObject>
#include <QList>
#include <QString>

namespace CommentClaims {

// One comment line: its 1-based line and its text without the marker.
struct CommentLine {
    int line = 0;
    QString text;
};

// Consecutive comment lines, or one block comment.
using Block = QList<CommentLine>;

// The comments of `text`, by the comment syntax its file name implies. A file
// of no known family has none.
QList<Block> harvest(const QString &fileName, const QString &text);

// The whole call below the resolved project root: the envelope, or a
// refusal {ok:false, code, error}.
QJsonObject run(const QString &rootCanonical, const QJsonObject &args);

} // namespace CommentClaims
