#include "reviewwip.h"

#include "roadmapparse.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

namespace reviewwip {

namespace {

QString withoutTrailingPeriod(QString s) {
    s = s.trimmed();
    if (s.endsWith(QLatin1Char('.'))) s.chop(1);
    return s.trimmed();
}

// A CHANGELOG bullet's summary: its first bold span, else its text less a
// trailing "(ID, ...)" citation.
QString bulletSummary(const QString &text) {
    const qsizetype open = text.indexOf(QStringLiteral("**"));
    if (open >= 0) {
        const qsizetype close = text.indexOf(QStringLiteral("**"), open + 2);
        const QString bold = text.mid(open + 2, close < 0 ? -1 : close - open - 2);
        if (!bold.trimmed().isEmpty()) return withoutTrailingPeriod(bold);
    }
    static const QRegularExpression citation(QStringLiteral("\\s*\\([^()]*\\)\\s*$"));
    QString plain = text;
    plain.remove(citation);
    return withoutTrailingPeriod(plain);
}

}  // namespace

QStringList lines(const QString &diff,
                  const QHash<QString, QString> &headlineById) {
    if (headlineById.isEmpty()) return {};
    // An id is a prefix, a hyphen and digits, not inside a longer token; the
    // headline map then decides whether it is one.
    static const QRegularExpression idToken(QStringLiteral(
        "(?<![A-Za-z0-9_])[A-Za-z][A-Za-z0-9_]*-S?\\d+(?![A-Za-z0-9_])"));
    static const QRegularExpression bulletHead(QStringLiteral("^[-*]\\s+(.*)$"));

    QStringList order;
    QHash<QString, QString> bulletFor;
    bool inChangelog = false;
    for (const QString &line : diff.split(QLatin1Char('\n'))) {
        if (line.startsWith(QStringLiteral("diff --git "))) {
            const qsizetype b = line.indexOf(QStringLiteral(" b/"));
            const QString path = b < 0 ? QString() : line.mid(b + 3).trimmed();
            inChangelog = QFileInfo(path).fileName()
                              .startsWith(QStringLiteral("changelog"), Qt::CaseInsensitive);
            continue;
        }
        if (!line.startsWith(QLatin1Char('+')) || line.startsWith(QStringLiteral("+++")))
            continue;
        const QString added = line.mid(1);
        QStringList ids;
        for (auto it = idToken.globalMatch(added); it.hasNext();) {
            const QString id = it.next().captured(0);
            if (!headlineById.contains(id) || ids.contains(id)) continue;
            ids << id;
            if (!order.contains(id)) order << id;
        }
        if (!inChangelog || ids.isEmpty()) continue;
        const auto head = bulletHead.match(added);
        if (!head.hasMatch()) continue;
        const QString summary = bulletSummary(head.captured(1));
        if (summary.isEmpty()) continue;
        for (const QString &id : ids)
            if (!bulletFor.contains(id)) bulletFor.insert(id, summary);
    }

    QStringList out;
    out.reserve(order.size());
    for (const QString &id : order)
        out << id + QStringLiteral(": ") + bulletFor.value(id, headlineById.value(id));
    return out;
}

QHash<QString, QString> headlines(const QString &roadmapPath) {
    // GUI-thread only, like the dialog that calls it. One entry: the dialog
    // shows one project at a time.
    struct Cached { QString path; qint64 size = -1; qint64 mtimeMs = 0;
                    QHash<QString, QString> map; };
    static Cached cache;
    const QFileInfo fi(roadmapPath);
    if (roadmapPath.isEmpty() || !fi.isFile()) return {};
    const qint64 size = fi.size();
    const qint64 mtimeMs = fi.lastModified().toMSecsSinceEpoch();
    if (cache.path == roadmapPath && cache.size == size && cache.mtimeMs == mtimeMs)
        return cache.map;

    QFile f(roadmapPath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QHash<QString, QString> map;
    const auto bullets = RoadmapParse::parseBullets(QString::fromUtf8(f.readAll()));
    for (const RoadmapParse::BulletRecord &rec : bullets) {
        if (rec.id.isEmpty() || rec.synthetic) continue;
        const QString headline = rec.headlineFull.isEmpty() ? rec.headline : rec.headlineFull;
        map.insert(rec.id, withoutTrailingPeriod(headline));
    }
    cache = Cached{roadmapPath, size, mtimeMs, map};
    return map;
}

}  // namespace reviewwip
