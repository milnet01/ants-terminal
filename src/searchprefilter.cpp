#include "searchprefilter.h"

#include <QSet>

#include <algorithm>

namespace SearchPrefilter {

namespace {

// A byte git and ripgrep fold the same way: printable ASCII, minus the two
// letters whose Unicode fold class leaves ASCII (K to U+212A, S to U+017F).
bool foldsAlike(char c) {
    if (c < 0x20 || c > 0x7e) return false;
    return c != 'k' && c != 'K' && c != 's' && c != 'S';
}

// One NUL-separated git listing as a set of raw paths.
QSet<QByteArray> pathSet(const QByteArray &nulSeparated) {
    QSet<QByteArray> out;
    for (const QByteArray &p : nulSeparated.split('\0'))
        if (!p.isEmpty()) out.insert(p);
    return out;
}

}  // namespace

QByteArray needle(const QByteArray &patternUtf8) {
    qsizetype bestStart = 0, bestLen = 0, start = 0;
    for (qsizetype i = 0; i <= patternUtf8.size(); ++i) {
        if (i < patternUtf8.size() && foldsAlike(patternUtf8.at(i))) continue;
        if (i - start > bestLen) {
            bestStart = start;
            bestLen = i - start;
        }
        start = i + 1;
    }
    return patternUtf8.mid(bestStart, bestLen);
}

QByteArray needleFile(const QByteArray &needle) {
    return needle + QByteArrayLiteral("\n\xff\xfe\n\xfe\xff\n");
}

int fallbackBudgetMs(int budgetMs) {
    constexpr int kTotalMs = 30000;   // kWorkspaceSearchMaxBudgetMs
    return std::min(budgetMs, kTotalMs - budgetMs);
}

QStringList skipList(const QByteArray &lsFilesTagged,
                     const QByteArray &grepHits,
                     const QByteArray &attributed) {
    const QSet<QByteArray> hits = pathSet(grepHits);
    const QSet<QByteArray> attrs = pathSet(attributed);

    // A path qualifies on one row reading `H <mode> <sha> 0`, with a regular
    // file's mode. Any other row for the same path disqualifies it: a second
    // `C` row is a modified file, and an unmerged path has a row per stage.
    QList<QByteArray> order;
    QSet<QByteArray> seen, rejected;
    for (const QByteArray &row : lsFilesTagged.split('\0')) {
        const qsizetype tab = row.indexOf('\t');
        if (tab < 0) continue;
        const QByteArray path = row.mid(tab + 1);
        const QList<QByteArray> head = row.left(tab).split(' ');
        const bool clean = head.size() == 4 && head.at(0) == "H"
            && (head.at(1) == "100644" || head.at(1) == "100755")
            && head.at(3) == "0";
        if (!clean || seen.contains(path)) rejected.insert(path);
        else order.append(path);
        seen.insert(path);
    }

    QStringList out;
    for (const QByteArray &path : std::as_const(order)) {
        if (rejected.contains(path) || hits.contains(path)
            || attrs.contains(path))
            continue;
        // A path that is not valid UTF-8 cannot be named in a glob argument,
        // so it stays in the scan.
        const QString text = QString::fromUtf8(path);
        if (text.toUtf8() != path) continue;
        out.append(text);
    }
    return out;
}

QString globEscape(const QString &path) {
    QString out;
    out.reserve(path.size() + 8);
    for (const QChar c : path) {
        if (QStringLiteral("[]*?{}\\").contains(c)) out += QLatin1Char('\\');
        out += c;
    }
    return out;
}

QStringList skipArgs(const QStringList &skip, qint64 capBytes) {
    QStringList out;
    qint64 used = 0;
    for (const QString &path : skip) {
        const QString glob = QStringLiteral("!/") + globEscape(path);
        used += 6 + glob.toUtf8().size();   // "--glob" plus the pattern
        if (used > capBytes) break;
        out << QStringLiteral("--glob") << glob;
    }
    return out;
}

}  // namespace SearchPrefilter
