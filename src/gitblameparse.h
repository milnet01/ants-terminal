#pragma once

// ANTS-5040 — parse `git blame --line-porcelain` output covering several
// `-L` ranges of one file, so a caller blames a file once rather than once
// per line.

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>

namespace GitBlame {

struct Entry {
    QString author;
    QString date;   // yyyy-MM-dd, local time, from author-time
    QString sha;    // first eight hex digits
};

// Final (current) line number -> entry, for every line the output covers.
// --line-porcelain repeats the commit headers for every line, so each entry
// is self-contained: a header line `<sha> <orig> <final> [<count>]`, then
// `key value` lines, then the line's content prefixed by a tab.
inline QHash<int, Entry> parseLinePorcelain(const QByteArray &out) {
    QHash<int, Entry> result;
    Entry cur;
    int finalLine = 0;
    bool inEntry = false;
    const QList<QByteArray> lines = out.split('\n');
    for (const QByteArray &ln : lines) {
        if (ln.startsWith('\t')) {
            if (inEntry && finalLine > 0)
                result.insert(finalLine, cur);
            inEntry = false;
            continue;
        }
        if (!inEntry) {
            const QList<QByteArray> parts = ln.split(' ');
            if (parts.size() >= 3 && parts[0].size() >= 40) {
                cur = Entry{};
                cur.sha = QString::fromLatin1(parts[0].left(8));
                finalLine = parts[2].toInt();
                inEntry = true;
            }
            continue;
        }
        if (ln.startsWith("author ")) {
            cur.author = QString::fromUtf8(ln.mid(7)).trimmed();
        } else if (ln.startsWith("author-time ")) {
            const qint64 t = ln.mid(12).trimmed().toLongLong();
            if (t > 0)
                cur.date = QDateTime::fromSecsSinceEpoch(t).toString(
                    QStringLiteral("yyyy-MM-dd"));
        }
    }
    return result;
}

}  // namespace GitBlame
