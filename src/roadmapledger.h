// ANTS-5516 — the pure half of roadmap_query mode:"ledger": which in-progress
// items have gone untouched, and which commits name no roadmap id. The verb
// gathers store rows and git output; everything that decides lives here, so
// it is testable without a store or a repository. Qt6::Core-only.
#pragma once

#include <QByteArray>
#include <QDate>
#include <QString>
#include <QVector>

namespace RoadmapLedger {

struct InProgressItem {
    QString id, headline;
    QString lastModified;   // YYYY-MM-DD as the store holds it; "" when unknown
};

struct StaleItem {
    QString id, headline, lastModified;
    int     days = 0;       // whole days since lastModified
};

// Items untouched for at least `staleDays` days before `today`, oldest first.
// An item with no readable date is stale: nothing shows anyone worked on it.
QVector<StaleItem> staleInProgress(const QVector<InProgressItem> &items,
                                   const QDate &today, int staleDays);

struct Commit { QString sha, subject; };

// `git log --format=%h%x09%s` output in, the commits whose subject names no
// `<idPrefix>-NNNN` id out, in log order. An empty prefix matches nothing, so
// every commit is returned.
QVector<Commit> commitsWithoutId(const QByteArray &gitLog, const QString &idPrefix);

// `git for-each-ref --format=%(refname:short) refs/tags` output in, the newest
// stable tag's name out ("v0.7.114"), or "" when there is none. A suffixed tag
// (`v1.0.0-rc1`) is not a release.
QString newestStableTag(const QByteArray &forEachRef);

}  // namespace RoadmapLedger
