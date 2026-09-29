// ANTS-4079 — item links: the parts of link handling that need no database.
// The rows themselves are RoadmapStore's (relateItems / relateCrossProject /
// relateDocument / linksFor / edgesOfType). Contract:
// docs/specs/ANTS-4079-item-links.md.
#pragma once

#include "roadmapstore.h"

#include <QJsonObject>
#include <QPair>
#include <QString>
#include <QVector>

namespace RoadmapLinks {

// The four types a caller writes with op:"link" (§ 2.1). The other two,
// relates-to and specified-by, are derived from the body.
bool isAuthored(const QString &type);

// The `links` key a row is reported under (§ 2.4): the forward name for the
// row's source, the reverse name for its target. relates-to is symmetric, so
// both directions report under relates_to.
QString keyFor(const QString &type, bool reverse);

// The `links` object for one item. Empty lists are omitted, so an item with
// no rows gets an empty object and the caller omits the key.
QJsonObject toJson(const QVector<RoadmapStore::LinkRow> &rows);

// The cycle adding src -> dst would close among `edges`, as the item pks
// src, dst, ..., each following the last; empty when it closes none.
QVector<qint64> cycleThrough(const QVector<QPair<qint64, qint64>> &edges,
                             qint64 src, qint64 dst);

}  // namespace RoadmapLinks
