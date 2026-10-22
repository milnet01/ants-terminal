// ANTS-5516 — see roadmapledger.h.
#include "roadmapledger.h"

#include <QRegularExpression>

#include <algorithm>
#include <climits>
#include <tuple>

namespace RoadmapLedger {

QVector<StaleItem> staleInProgress(const QVector<InProgressItem> &items,
                                   const QDate &today, int staleDays) {
    QVector<StaleItem> out;
    for (const InProgressItem &it : items) {
        const QDate d = QDate::fromString(it.lastModified.left(10), Qt::ISODate);
        const int days = d.isValid() ? int(d.daysTo(today)) : -1;
        if (d.isValid() && days < staleDays) continue;
        out.push_back({it.id, it.headline, it.lastModified, days});
    }
    // Oldest first; an undated item sorts first, as the least-known one.
    std::stable_sort(out.begin(), out.end(), [](const StaleItem &a, const StaleItem &b) {
        const int ka = a.days < 0 ? INT_MAX : a.days;
        const int kb = b.days < 0 ? INT_MAX : b.days;
        return ka > kb;
    });
    return out;
}

QVector<Commit> commitsWithoutId(const QByteArray &gitLog, const QString &idPrefix) {
    // Not idTokenPattern(): it matches THIS project's prefix only, and no
    // synthesised (`-S`) id.
    const QRegularExpression rx(QStringLiteral("\\b") + QRegularExpression::escape(idPrefix) +
                                QStringLiteral("-\\d+\\b"));
    QVector<Commit> out;
    for (const QByteArray &raw : gitLog.split('\n')) {
        const QString line = QString::fromUtf8(raw);
        const int tab = line.indexOf(QLatin1Char('\t'));
        if (tab <= 0) continue;
        const QString subject = line.mid(tab + 1);
        if (!idPrefix.isEmpty() && rx.match(subject).hasMatch()) continue;
        out.push_back({line.left(tab), subject});
    }
    return out;
}

QString newestStableTag(const QByteArray &forEachRef) {
    static const QRegularExpression stable(QStringLiteral("^v?(\\d+)\\.(\\d+)\\.(\\d+)$"));
    QString best;
    std::tuple<int, int, int> bestKey{-1, -1, -1};
    for (const QByteArray &raw : forEachRef.split('\n')) {
        const QString name = QString::fromUtf8(raw).trimmed();
        const auto m = stable.match(name);
        if (!m.hasMatch()) continue;
        const std::tuple<int, int, int> k{m.captured(1).toInt(), m.captured(2).toInt(),
                                          m.captured(3).toInt()};
        if (k > bestKey) { bestKey = k; best = name; }
    }
    return best;
}

}  // namespace RoadmapLedger
