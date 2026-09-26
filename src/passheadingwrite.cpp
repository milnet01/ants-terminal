// ANTS-2126 — implementation of the pass-headings writer helpers.
// See passheadingwrite.h + docs/specs/ANTS-2126.md.

#include "passheadingwrite.h"

#include "roadmapparse.h"   // ANTS-3768 — the format's status vocabulary

#include <QDate>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>

#include <utility>

namespace PassHeadingWrite {

namespace {

// ANTS-3768 — the four status glyphs come from RoadmapParse, the reader's own
// header, rather than being spelled again here. This file used to define them
// as UTF-8 byte escapes under a comment promising they matched the reader, and
// that comment was the tell: a hand-maintained "kept in step with" note is
// exactly what a shared constant makes unnecessary. One definition of the
// format's vocabulary, so the writer cannot drift from the reader silently.
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

// ANTS-5395 — a thematic break (`---`, `***`, `___`, three or more of one
// character, spaces allowed). A pass block commonly ends in one to separate
// it from the next pass, and it belongs to the layout, not the item's text.
bool isRule(const QString &line) {
    const QString t = QString(line).remove(QChar(' ')).trimmed();
    if (t.size() < 3) return false;
    const QChar c = t.at(0);
    if (c != u'-' && c != u'*' && c != u'_') return false;
    for (const QChar x : t)
        if (x != c) return false;
    return true;
}

// ANTS-5395 — where a note goes in lines [first, end): after the last line
// with content that is not a trailing rule, so the note stays inside the
// item rather than landing after its separator. `fallback` when the span
// holds nothing else.
int noteInsertionPoint(const QStringList &lines, int first, int end, int fallback) {
    int last = end - 1;
    while (last >= first && lines.at(last).trimmed().isEmpty()) --last;
    if (last >= first && isRule(lines.at(last))) {
        --last;
        while (last >= first && lines.at(last).trimmed().isEmpty()) --last;
    }
    return last >= first ? last + 1 : fallback;
}

QString capitaliseFirst(const QString &s) {
    if (s.isEmpty()) return s;
    return s.left(1).toUpper() + s.mid(1);
}

// Rewrite the word of the first Status line in lines[from..] that classifies —
// the reader's line — to `keyword`, and a status glyph to `emoji`, keeping its
// date and prose. False when none classifies.
bool rewriteStatusWord(QStringList &lines, int from, const QString &keyword,
                       const QString &emoji);

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
    if (roadmapStatus == QStringLiteral("dropped"))     return QStringLiteral("dropped");  // ANTS-4977
    return QString();
}

QString passStatusEmoji(const QString &keyword) {
    // The constants are `const char *`; the callers here want QString.
    if (keyword == QStringLiteral("todo"))
        return QString::fromUtf8(RoadmapParse::kEmojiPlanned);
    if (keyword == QStringLiteral("in-progress"))
        return QString::fromUtf8(RoadmapParse::kEmojiInProgress);
    if (keyword == QStringLiteral("done"))
        return QString::fromUtf8(RoadmapParse::kEmojiDone);
    if (keyword == QStringLiteral("deferred"))
        return QString::fromUtf8(RoadmapParse::kEmojiConsidered);
    if (keyword == QStringLiteral("dropped"))                               // ANTS-4977
        return QString::fromUtf8(RoadmapParse::kEmojiDropped);
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

QString designatorFromPassId(const QString &id) {
    // Inverse of passIdFromDesignator. The store keeps only the synthesised
    // id, so the render recovers the designator from it — a pass block cannot
    // be emitted without one, and inventing a second place to keep it would be
    // a second thing to keep in step.
    static const QRegularExpression re(
        QStringLiteral("^PASS-(\\d+)-(\\d+)(?:-([A-Za-z][A-Za-z0-9]*))?$"));
    const QRegularExpressionMatch m = re.match(id);
    if (!m.hasMatch()) return QString();
    const QString sub = m.captured(3);
    return sub.isEmpty()
        ? QStringLiteral("%1.%2").arg(m.captured(1), m.captured(2))
        : QStringLiteral("%1.%2.%3").arg(m.captured(1), m.captured(2), sub);
}

namespace {

bool rewriteStatusWord(QStringList &lines, int from, const QString &keyword,
                       const QString &emoji) {
    for (int j = from; j < lines.size(); ++j) {
        const QRegularExpressionMatch pm = rxStatusPrefix().match(lines.at(j));
        if (!pm.hasMatch()) continue;
        const QString value = pm.captured(2);
        const QRegularExpressionMatch vm = rxStatusValue().match(value);
        const QString lead = vm.captured(1), word = vm.captured(2);
        // A leading run that is not a status glyph (`**`) is the author's
        // decoration: kept, never replaced by an emoji.
        bool glyph = false;
        for (const char *k : {"todo", "in-progress", "done", "deferred", "dropped"})
            glyph = glyph || lead == passStatusEmoji(QLatin1String(k));
        if (word.isEmpty() && !glyph) continue;   // content-free: the reader skips it too
        const qsizetype gap = vm.capturedStart(2) - lead.size();
        lines[j] = pm.captured(1)
                 + (glyph ? emoji : lead)
                 + value.mid(lead.size(), gap)
                 + (word.isEmpty() ? QString()
                                   : (glyph ? capitaliseFirst(keyword) : keyword))
                 + value.mid(vm.capturedEnd(0));
        return true;
    }
    return false;
}

}  // namespace

QString formatPassBlock(const QString &pass, const QString &headline,
                        const QString &keyword, const QString &body) {
    QString head = QStringLiteral("#### Pass %1").arg(pass);
    const QString tail = headline.trimmed();
    if (!tail.isEmpty()) head += QChar(' ') + tail;

    // ANTS-5231 — the author's Status line IS the status slot. The migration
    // stores it in the body, so emitting a canonical line beside it added a
    // copy on every render. The REAL reader decides whether the body declares
    // a status and what it means, so this cannot disagree with it.
    if (!body.isEmpty()) {
        QStringList block = body.split(QChar('\n'));
        block.prepend(head);
        const auto rec = RoadmapParse::parsePassHeadingBlock(block);
        if (rec && !rec->sourceStatus.isEmpty()) {
            // Already means the item's status: byte for byte, so `planned`
            // stays `planned` rather than churning to `todo` on every render.
            if (rec->status == passStatusEmoji(keyword))
                return block.join(QChar('\n'));
            if (rewriteStatusWord(block, 1, keyword, passStatusEmoji(keyword)))
                return block.join(QChar('\n'));
        }
    }

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

    // Scan the block for the first `- **Status**:` line, bounded by the next
    // heading (level ≤ 4) or EOF — the same span as the reader
    // (RoadmapParse::parsePassHeadingBullets, roadmapparse.cpp). ANTS-5337:
    // both were capped at 50 lines, which missed a late line and inserted a
    // second one here.
    int statusLine = -1;
    for (int j = head + 1; j < lines.size(); ++j) {
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
    r.matchedId = std::move(matchedId);
    r.matchedHeadline = std::move(matchedTail);
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
    // ANTS-5395 — and above a trailing `---` rule, which separates passes.
    // ANTS-5404 — as a bullet, never a bare line.
    const QStringList noteLines =
        formatPassNote(note, PassNoteKind::Progress,
                       QDate::currentDate().toString(Qt::ISODate))
            .split(QChar('\n'));
    const int insertAt = noteInsertionPoint(lines, head + 1, blockEnd, head + 1);
    for (int k = noteLines.size() - 1; k >= 0; --k) {
        lines.insert(insertAt, noteLines.at(k));
    }

    r.ok = true;
    r.markdown = lines.join(QChar('\n'));
    r.matchedId = std::move(matchedId);
    r.matchedHeadline = std::move(matchedTail);
    r.headingLine = head;
    r.changedLine = insertAt;
    return r;
}

QString insertPassNote(const QString &body, const QString &note) {
    if (note.isEmpty()) return body;
    QStringList lines = body.split(QChar('\n'));
    if (body.isEmpty()) lines.clear();
    const int at = noteInsertionPoint(lines, 0, int(lines.size()), 0);
    const QStringList noteLines = note.split(QChar('\n'));
    for (int k = int(noteLines.size()) - 1; k >= 0; --k)
        lines.insert(at, noteLines.at(k));
    return lines.join(QChar('\n'));
}

QString redatePassStatus(const QString &body, const QString &isoDate) {
    // `<word> (YYYY-MM-DD` right after the status word; the rest of the line,
    // `). Lanes: ...` included, is kept byte for byte.
    static const QRegularExpression rxDate(
        QStringLiteral("^([^\\sA-Za-z0-9_-]*\\s*[A-Za-z-]+\\s*\\()"
                       "\\d{4}-\\d{2}-\\d{2}"));
    QStringList lines = body.split(QChar('\n'));
    for (QString &line : lines) {
        const QRegularExpressionMatch pm = rxStatusPrefix().match(line);
        if (!pm.hasMatch()) continue;
        const QString value = pm.captured(2);
        const QRegularExpressionMatch dm = rxDate.match(value);
        if (dm.hasMatch())
            line = pm.captured(1) + dm.captured(1) + isoDate
                 + value.mid(dm.capturedEnd(0));
        break;   // the FIRST Status line is the item's, as for the reader
    }
    return lines.join(QChar('\n'));
}

QString formatPassNote(const QString &note, PassNoteKind kind,
                       const QString &isoDate) {
    QStringList lines = note.split(QChar('\n'));
    if (lines.first().startsWith(QStringLiteral("- ")))
        return note;   // the caller wrote its own bullet
    const QString label = kind == PassNoteKind::Resolution
        ? QStringLiteral("Resolution") : QStringLiteral("Progress");
    lines[0] = QStringLiteral("- **%1** (%2): %3").arg(label, isoDate, lines.first());
    for (int k = 1; k < lines.size(); ++k)
        if (!lines.at(k).isEmpty() && !lines.at(k).front().isSpace())
            lines[k] = QStringLiteral("  ") + lines.at(k);
    return lines.join(QChar('\n'));
}

QString insertPassNoteAboveStatus(const QString &body, const QString &bullet) {
    QStringList lines = body.split(QChar('\n'));
    for (int j = 0; j < lines.size(); ++j) {
        if (!rxStatusPrefix().match(lines.at(j)).hasMatch()) continue;
        const QStringList add = bullet.split(QChar('\n'));
        for (int k = int(add.size()) - 1; k >= 0; --k)
            lines.insert(j, add.at(k));
        return lines.join(QChar('\n'));
    }
    return insertPassNote(body, bullet);
}

QString passStatusWordFor(const QString &canonicalKeyword,
                          const QStringList &bodies) {
    const QString want = passStatusEmoji(canonicalKeyword);
    QStringList order;                 // first-seen order breaks a tie
    QHash<QString, int> count;
    for (const QString &body : bodies) {
        QString value;
        for (const QString &line : body.split(QChar('\n'))) {
            const QRegularExpressionMatch pm = rxStatusPrefix().match(line);
            if (pm.hasMatch()) { value = pm.captured(2); break; }
        }
        if (value.isEmpty()) continue;
        // Classify with the reader itself, so a word counts here exactly
        // when the reader would read it as this status.
        const auto rec = RoadmapParse::parsePassHeadingBlock(
            {QStringLiteral("#### Pass 0.0"), QStringLiteral("- **Status**: ") + value});
        if (!rec || rec->status != want) continue;
        const QString word = rxStatusValue().match(value.trimmed()).captured(2);
        if (word.isEmpty()) continue;
        if (!count.contains(word)) order.append(word);
        ++count[word];
    }
    QString best = canonicalKeyword;
    int bestCount = 0;
    for (const QString &w : std::as_const(order))
        if (count.value(w) > bestCount) { best = w; bestCount = count.value(w); }
    return best;
}

QString setPassStatusWord(const QString &body, const QString &word,
                          const QString &canonicalKeyword) {
    QStringList lines = body.split(QChar('\n'));
    rewriteStatusWord(lines, 0, word, passStatusEmoji(canonicalKeyword));
    return lines.join(QChar('\n'));
}

}  // namespace PassHeadingWrite
