// ANTS-2126 — implementation of the pass-headings writer helpers.
// See passheadingwrite.h + docs/specs/ANTS-2126.md.

#include "passheadingwrite.h"

#include <QRegularExpression>
#include <QStringList>
#include <algorithm>

namespace PassHeadingWrite {

namespace {

// The four canonical status glyphs (UTF-8 byte sequences, matching the
// reader). ANTS-3764 moved the reader to roadmapparse.{h,cpp} and exported
// the same four as RoadmapParse::kEmojiDone / kEmojiPlanned /
// kEmojiInProgress / kEmojiConsidered, so these are now a duplicate of a
// header this file could include — see ANTS-3768.
QString glyphTodo()       { return QString::fromUtf8("\xF0\x9F\x93\x8B"); } // 📋
QString glyphInProgress() { return QString::fromUtf8("\xF0\x9F\x9A\xA7"); } // 🚧
QString glyphDone()       { return QString::fromUtf8("\xE2\x9C\x85");     } // ✅
QString glyphDeferred()   { return QString::fromUtf8("\xF0\x9F\x92\xAD"); } // 💭

// `#### Pass <major>.<minor>[.<sub>] (meta) <tail>` — MUST stay in sync
// with the reader's rxHead (roadmapdialog.cpp parsePassHeadingBullets,
// ANTS-1530/2035/2039). The write-side INV-12 round-trip test is the
// guard against drift: a write rendered here is re-parsed by the real
// reader, so any divergence in this pattern fails that test.
const QRegularExpression &rxHead() {
    static const QRegularExpression re(
        QStringLiteral("^####\\s+Pass\\s+(\\d+)\\.(\\d+)"
                       "(?:\\.([A-Za-z][A-Za-z0-9]*))?\\s*"
                       "(?:\\(([^)]*)\\))?\\s*(.*?)\\s*$"));
    return re;
}

// Split a `- **Status**:` line into (prefix, value). Prefix is the
// indentation + list marker + `**Status**:` + trailing spaces; value is
// everything after. Mirrors the reader's rxStatusLine anchor.
const QRegularExpression &rxStatusPrefix() {
    static const QRegularExpression re(
        QStringLiteral("^(\\s*[-*]\\s*\\*\\*Status\\*\\*\\s*:\\s*)(.*)$"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// Classify a Status value into (emoji, keyword) — same split the reader
// uses: a leading non-word, non-space glyph run then a keyword token.
const QRegularExpression &rxStatusValue() {
    static const QRegularExpression re(
        QStringLiteral("^([^\\sA-Za-z0-9_-]+)?\\s*([A-Za-z0-9_-]*)"));
    return re;
}

bool isHeadingLeQ4(const QString &line) {
    // A heading of level 1-4 (the reader's block boundary — a level-5+
    // `#####` does NOT close a pass block).
    return line.startsWith(QChar('#')) &&
           !line.startsWith(QStringLiteral("#####"));
}

QString capitaliseFirst(const QString &s) {
    if (s.isEmpty()) return s;
    return s.left(1).toUpper() + s.mid(1);
}

// Synthesise the reader's id for a `#### Pass` heading match.
QString idForHead(const QRegularExpressionMatch &m) {
    const int major   = m.captured(1).toInt();
    const int minor   = m.captured(2).toInt();
    const QString sub = m.captured(3).trimmed();
    return sub.isEmpty()
        ? QStringLiteral("PASS-%1-%2").arg(major).arg(minor)
        : QStringLiteral("PASS-%1-%2-%3").arg(major).arg(minor).arg(sub);
}

QString normaliseHeadline(const QString &s) {
    return s.simplified().toLower();
}

// Find the 0-based line index of the pass that matches the locator
// (id wins; else headline tail). -1 if none. `outId`/`outTail` receive
// the matched pass's synthesised id + heading tail.
int locatePass(const QStringList &lines, const QString &locatorId,
               const QString &locatorHeadline, QString *outId,
               QString *outTail) {
    const QString wantHeadline = normaliseHeadline(locatorHeadline);
    for (int i = 0; i < lines.size(); ++i) {
        const QRegularExpressionMatch m = rxHead().match(lines.at(i));
        if (!m.hasMatch()) continue;
        const QString id   = idForHead(m);
        const QString tail = m.captured(5).trimmed();
        bool hit = false;
        if (!locatorId.isEmpty()) {
            hit = (id == locatorId);
        } else if (!locatorHeadline.isEmpty()) {
            hit = (normaliseHeadline(tail) == wantHeadline);
        }
        if (hit) {
            if (outId)   *outId   = id;
            if (outTail) *outTail = tail;
            return i;
        }
    }
    return -1;
}

}  // namespace

QString passStatusKeyword(const QString &roadmapStatus) {
    if (roadmapStatus == QStringLiteral("planned"))     return QStringLiteral("todo");
    if (roadmapStatus == QStringLiteral("in-progress")) return QStringLiteral("in-progress");
    if (roadmapStatus == QStringLiteral("shipped"))     return QStringLiteral("done");
    if (roadmapStatus == QStringLiteral("considered"))  return QStringLiteral("deferred");
    return QString();
}

QString passStatusEmoji(const QString &keyword) {
    if (keyword == QStringLiteral("todo"))        return glyphTodo();
    if (keyword == QStringLiteral("in-progress")) return glyphInProgress();
    if (keyword == QStringLiteral("done"))        return glyphDone();
    if (keyword == QStringLiteral("deferred"))    return glyphDeferred();
    return QString();
}

bool isValidPassDesignator(const QString &pass) {
    static const QRegularExpression re(
        QStringLiteral("^\\d+\\.\\d+(?:\\.[A-Za-z][A-Za-z0-9]*)?$"));
    return re.match(pass).hasMatch();
}

QString passIdFromDesignator(const QString &pass) {
    static const QRegularExpression re(
        QStringLiteral("^(\\d+)\\.(\\d+)(?:\\.([A-Za-z][A-Za-z0-9]*))?$"));
    const QRegularExpressionMatch m = re.match(pass);
    if (!m.hasMatch()) return QString();
    const int major   = m.captured(1).toInt();
    const int minor   = m.captured(2).toInt();
    const QString sub = m.captured(3);
    return sub.isEmpty()
        ? QStringLiteral("PASS-%1-%2").arg(major).arg(minor)
        : QStringLiteral("PASS-%1-%2-%3").arg(major).arg(minor).arg(sub);
}

QString formatPassBlock(const QString &pass, const QString &headline,
                        const QString &keyword, const QString &body) {
    QString head = QStringLiteral("#### Pass %1").arg(pass);
    const QString tail = headline.trimmed();
    if (!tail.isEmpty()) head += QChar(' ') + tail;
    QString out = head + QChar('\n') +
                  QStringLiteral("- **Status**: ") + keyword;
    if (!body.isEmpty()) out += QChar('\n') + body;
    return out;
}

WriteResult flipPassStatus(const QString &markdown,
                           const QString &locatorId,
                           const QString &locatorHeadline,
                           const QString &keyword) {
    WriteResult r;
    QStringList lines = markdown.split(QChar('\n'));
    QString matchedId, matchedTail;
    const int head = locatePass(lines, locatorId, locatorHeadline,
                                &matchedId, &matchedTail);
    if (head < 0) {
        r.code = QStringLiteral("bullet_not_found");
        return r;
    }

    // Scan the lookahead window for the first `- **Status**:` line,
    // bounded by the next heading (level ≤ 4) or 50 lines — same window
    // as the reader (RoadmapParse::parsePassHeadingBullets, roadmapparse.cpp).
    const int probeCap = std::min<int>(lines.size(), head + 51);
    int statusLine = -1;
    for (int j = head + 1; j < probeCap; ++j) {
        if (isHeadingLeQ4(lines.at(j))) break;
        if (rxStatusPrefix().match(lines.at(j)).hasMatch()) {
            statusLine = j;
            break;
        }
    }

    const QString emoji = passStatusEmoji(keyword);
    if (statusLine < 0) {
        // INV-6 — no Status line in the window: insert one (keyword form)
        // directly under the heading.
        lines.insert(head + 1, QStringLiteral("- **Status**: ") + keyword);
        r.changedLine = head + 1;
    } else {
        // INV-5 — rewrite the value preserving the line's style.
        const QRegularExpressionMatch pm =
            rxStatusPrefix().match(lines.at(statusLine));
        const QString prefix = pm.captured(1);
        const QRegularExpressionMatch vm =
            rxStatusValue().match(pm.captured(2).trimmed());
        const bool hasEmoji   = !vm.captured(1).isEmpty();
        const bool hasKeyword = !vm.captured(2).isEmpty();
        QString newValue;
        if (hasEmoji && hasKeyword) {
            newValue = emoji + QChar(' ') + capitaliseFirst(keyword);
        } else if (hasEmoji) {
            newValue = emoji;
        } else {
            newValue = keyword;
        }
        lines[statusLine] = prefix + newValue;
        r.changedLine = statusLine;
    }

    r.ok = true;
    r.markdown = lines.join(QChar('\n'));
    r.matchedId = matchedId;
    r.matchedHeadline = matchedTail;
    r.headingLine = head;
    return r;
}

WriteResult annotatePass(const QString &markdown,
                         const QString &locatorId,
                         const QString &locatorHeadline,
                         const QString &note) {
    WriteResult r;
    QStringList lines = markdown.split(QChar('\n'));
    QString matchedId, matchedTail;
    const int head = locatePass(lines, locatorId, locatorHeadline,
                                &matchedId, &matchedTail);
    if (head < 0) {
        r.code = QStringLiteral("bullet_not_found");
        return r;
    }

    // Block end = the next heading (level ≤ 4) or EOF.
    int blockEnd = lines.size();
    for (int j = head + 1; j < lines.size(); ++j) {
        if (isHeadingLeQ4(lines.at(j))) { blockEnd = j; break; }
    }
    // Append after the block's last non-blank content line, so the note
    // lands inside the block rather than after its trailing blank lines.
    int lastContent = head;
    for (int j = head + 1; j < blockEnd; ++j) {
        if (!lines.at(j).trimmed().isEmpty()) lastContent = j;
    }
    const QStringList noteLines = note.split(QChar('\n'));
    const int insertAt = lastContent + 1;
    for (int k = noteLines.size() - 1; k >= 0; --k) {
        lines.insert(insertAt, noteLines.at(k));
    }

    r.ok = true;
    r.markdown = lines.join(QChar('\n'));
    r.matchedId = matchedId;
    r.matchedHeadline = matchedTail;
    r.headingLine = head;
    r.changedLine = insertAt;
    return r;
}

}  // namespace PassHeadingWrite
