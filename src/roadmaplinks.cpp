// ANTS-4079 — item links. See roadmaplinks.h.
#include "roadmaplinks.h"

#include <QHash>
#include <QJsonArray>
#include <QMultiHash>
#include <QQueue>

namespace RoadmapLinks {

bool isAuthored(const QString &type) {
    return type == QLatin1String("splits-from") || type == QLatin1String("blocked-by") ||
           type == QLatin1String("duplicate-of") || type == QLatin1String("supersedes");
}

QString keyFor(const QString &type, bool reverse) {
    if (type == QLatin1String("splits-from"))
        return reverse ? QStringLiteral("parts") : QStringLiteral("splits_from");
    if (type == QLatin1String("blocked-by"))
        return reverse ? QStringLiteral("blocks") : QStringLiteral("blocked_by");
    if (type == QLatin1String("duplicate-of"))
        return reverse ? QStringLiteral("duplicated_by") : QStringLiteral("duplicate_of");
    if (type == QLatin1String("supersedes"))
        return reverse ? QStringLiteral("superseded_by") : QStringLiteral("supersedes");
    if (type == QLatin1String("relates-to"))
        return QStringLiteral("relates_to");
    if (type == QLatin1String("specified-by"))
        return QStringLiteral("specified_by");
    return QString();
}

QJsonObject toJson(const QVector<RoadmapStore::LinkRow> &rows) {
    QJsonObject out;
    for (const auto &r : rows) {
        const QString key = keyFor(r.type, r.reverse);
        if (key.isEmpty())
            continue;
        QJsonArray list = out.value(key).toArray();
        const QString value = r.path.isEmpty() ? r.id : r.path;
        if (!list.contains(value))
            list.append(value);
        out.insert(key, list);
    }
    return out;
}

QVector<qint64> cycleThrough(const QVector<QPair<qint64, qint64>> &edges,
                             qint64 src, qint64 dst) {
    if (src == dst)
        return {src};
    QMultiHash<qint64, qint64> next;
    for (const auto &e : edges)
        next.insert(e.first, e.second);

    // Breadth-first from dst: the first path back to src is a shortest cycle,
    // which is the one worth naming.
    QHash<qint64, qint64> parent;
    QQueue<qint64> queue;
    queue.enqueue(dst);
    parent.insert(dst, dst);
    while (!queue.isEmpty()) {
        const qint64 at = queue.dequeue();
        for (auto it = next.constFind(at); it != next.constEnd() && it.key() == at; ++it) {
            const qint64 to = it.value();
            if (parent.contains(to))
                continue;
            parent.insert(to, at);
            if (to == src) {
                QVector<qint64> back;   // src, ..., dst walking parents
                for (qint64 n = src; n != dst; n = parent.value(n))
                    back.append(n);
                back.append(dst);
                // Reported from src: src, dst, ..., the node before src.
                QVector<qint64> cycle{src};
                for (qsizetype i = back.size() - 1; i > 0; --i)
                    cycle.append(back.at(i));
                return cycle;
            }
            queue.enqueue(to);
        }
    }
    return {};
}

}  // namespace RoadmapLinks
