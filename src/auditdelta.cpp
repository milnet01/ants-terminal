// ANTS-1870 — pure since-last-run findings delta. See auditdelta.h.

#include "auditdelta.h"

#include <QJsonObject>
#include <QJsonValue>

namespace AuditDelta {

namespace {

QString fpOf(const QJsonValue &v) {
    return v.toObject().value(QStringLiteral("fp")).toString();
}
QString fileOf(const QJsonValue &v) {
    return v.toObject().value(QStringLiteral("file")).toString();
}
QSet<QString> fpSet(const QJsonArray &a) {
    QSet<QString> s;
    s.reserve(a.size());
    for (const QJsonValue &v : a) {
        const QString fp = fpOf(v);
        if (!fp.isEmpty()) s.insert(fp);
    }
    return s;
}

}  // namespace

DeltaResult computeDelta(const QJsonArray &current,
                         const QJsonArray &prior,
                         const QSet<QString> &changedFiles) {
    DeltaResult d;

    // 1. Partition prior by whether its file was re-scanned this run.
    //    Only priorOnChanged can yield added/removed — an untouched file
    //    was not re-scanned, so its prior findings are assumed still present.
    QJsonArray priorOnChanged;
    QJsonArray priorOnUntouched;
    for (const QJsonValue &v : prior) {
        if (changedFiles.contains(fileOf(v))) priorOnChanged.append(v);
        else                                  priorOnUntouched.append(v);
    }

    const QSet<QString> currentFps        = fpSet(current);
    const QSet<QString> priorChangedFps   = fpSet(priorOnChanged);

    // 2. added = current ∖ priorOnChanged.
    for (const QJsonValue &v : current)
        if (!priorChangedFps.contains(fpOf(v))) d.added.append(v);

    // 3. removed = priorOnChanged ∖ current.
    for (const QJsonValue &v : priorOnChanged)
        if (!currentFps.contains(fpOf(v))) d.removed.append(v);

    // 4. carriedForward = (current ∩ priorOnChanged) ∪ priorOnUntouched.
    for (const QJsonValue &v : current)
        if (priorChangedFps.contains(fpOf(v))) d.carriedForward.append(v);
    for (const QJsonValue &v : priorOnUntouched)
        d.carriedForward.append(v);

    // 5. merged = current ∪ priorOnUntouched — the whole-tree set the
    //    carry-forward SARIF + recorded sidecar both persist.
    for (const QJsonValue &v : current)         d.merged.append(v);
    for (const QJsonValue &v : priorOnUntouched) d.merged.append(v);

    d.addedCount          = d.added.size();
    d.removedCount        = d.removed.size();
    d.carriedForwardCount = d.carriedForward.size();
    return d;
}

}  // namespace AuditDelta
