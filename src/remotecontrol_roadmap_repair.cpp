// ANTS-4585 phase 2 — TU 14/16 — roadmap_log op:"repair_trailers": recover the
// trailer values migration cut short, by re-parsing the prose that still
// holds them.
// Contract: tests/features/roadmap_repair_trailers/spec.md
//
// Its own TU for remotecontrol_roadmap_backfill.cpp's reason: it is a member
// that never existed in the pre-split remotecontrol_roadmap_log.cpp, so no
// pre-split relative order can be violated by appending it last.
//
// The pass writes nothing it derived from the render. A bullet is rebuilt from
// its head line and its STORED body alone, because RoadmapRender::bulletText()
// composes a trailer line for every column the prose does not declare
// (ANTS-4599) — re-parsing that hands the column straight back and the repair
// becomes a no-op that reports success.

#include "remotecontrol.h"

#include "roadmapparse.h"
#include "roadmapstore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace {

// The format's own two-space continuation indent. parseAntsV1Bullet() reads a
// bullet, not a body, so the body has to go back under the hang it was stored
// without (ANTS-4558 dedents it on the way out).
QString rlIndentBody(const QString &body) {
    QStringList out;
    const QStringList lines = body.split(QLatin1Char('\n'));
    out.reserve(lines.size());
    for (const QString &l : lines)
        out.append(QStringLiteral("  ") + l);
    return out.join(QLatin1Char('\n'));
}

// THE GUARD (spec § Contract). Write only where the stored value is a strict
// prefix of the re-parse, so the pass can only ever EXTEND a value with more of
// the author's own adjacent prose.
//
// Everything else is skipped, and the case that matters is a stored value that
// is NEWER than the prose: roadmap_log updates the column and leaves the legacy
// inline run alone, so seven measured items (ANTS-1933, -1934, -1931, -1932,
// -1884, -1565, -1579) hold text the prose has never caught up with. Reverting
// those is unrecoverable — the prose is the only other copy and it is the stale
// one.
bool rlIsStrictExtension(const QString &stored, const QString &reparsed) {
    return !stored.isEmpty() && reparsed.size() > stored.size()
           && reparsed.startsWith(stored);
}

// Element-wise, never on a joined string: joining to compare diverges on
// separator spacing alone, which is a difference in the comparison rather than
// in the data (ANTS-4505 made the same call for the render's presence test).
bool rlIsStrictExtension(const QStringList &stored, const QStringList &reparsed) {
    if (stored.isEmpty() || reparsed.size() <= stored.size())
        return false;
    for (qsizetype i = 0; i < stored.size(); ++i)
        if (stored.at(i) != reparsed.at(i))
            return false;
    return true;
}

QString rlLanesJson(const QStringList &lanes) {
    QJsonArray a;
    for (const QString &l : lanes)
        a.append(l);
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}

struct RlRepairWrite {
    qint64 pk = 0;
    QString field;   // "layman" | "source" | "lanes"
    QString value;   // already in the column's stored form
};

} // namespace

QJsonDocument RemoteControl::cmdRoadmapLogRepairTrailers(const QJsonObject &req) {
    QString root, roadmapPath;
    QJsonDocument refusal;
    const auto target = roadmapSectionOpTarget(req, &root, &roadmapPath, &refusal);
    if (!target) {
        // Remapped for ANTS-4501's reason: the shared prologue calls this
        // `op_unsupported`, and a caller branching on `code` should get the one
        // this op's contract promises.
        QJsonObject env = refusal.object();
        if (env.value(QStringLiteral("code")).toString()
                == QLatin1String("op_unsupported")) {
            env[QStringLiteral("code")]  = QStringLiteral("project_not_registered");
            env[QStringLiteral("error")] = QStringLiteral(
                "roadmap_log: repair_trailers needs a store-migrated project — "
                "the store holds no row for \"%1\". Run roadmap_migrate first.")
                    .arg(root.isEmpty() ? roadmapPath : root);
            return QJsonDocument(env);
        }
        return refusal;
    }
    RoadmapStore &store    = *target->store;
    const qint64 projectId = target->projectId;
    const bool dryRun      = req.value(QStringLiteral("dry_run")).toBool();

    const auto rpErr = [](const QString &code, const QString &message) {
        QJsonObject env;
        env[QStringLiteral("ok")]    = false;
        env[QStringLiteral("code")]  = code;
        env[QStringLiteral("error")] = message;
        return QJsonDocument(env);
    };

    QString err;
    const auto items = store.readItems(projectId, &err);
    if (!items)
        return rpErr(QStringLiteral("store_failed"), err);

    // Decide every write before opening a transaction, so `dry_run` reports the
    // real run's counts rather than a second estimate of them (INV-5).
    QVector<RlRepairWrite> plan;
    int scanned = 0, withRun = 0, skipped = 0;
    int laymanFixed = 0, sourceFixed = 0, lanesFixed = 0;
    qint64 charsRecovered = 0;
    QStringList skippedIds;

    for (auto it = items->constBegin(); it != items->constEnd(); ++it) {
        const qint64 pk = it.key();
        const RoadmapStore::ItemWrite &w = it.value();
        ++scanned;
        if (w.body.trimmed().isEmpty())
            continue;

        const QString bullet = QStringLiteral("- ")
            + QString::fromUtf8(RoadmapParse::kEmojiPlanned)
            + QStringLiteral(" [") + w.id + QStringLiteral("] **")
            + w.headline.simplified() + QStringLiteral("**\n")
            + rlIndentBody(w.body);
        const auto rec = RoadmapParse::parseAntsV1Bullet(bullet);
        if (!rec)
            continue;

        const bool runHere = !rec->layman.isEmpty() || !rec->source.isEmpty()
                             || !rec->lanes.isEmpty();
        if (runHere)
            ++withRun;

        bool skippedHere = false;
        const auto consider = [&](const QString &field, const QString &stored,
                                  const QString &reparsed, int *counter) {
            if (reparsed.isEmpty() || reparsed == stored)
                return;
            if (!rlIsStrictExtension(stored, reparsed)) {
                skippedHere = true;
                return;
            }
            plan.push_back({pk, field, reparsed});
            charsRecovered += reparsed.size() - stored.size();
            ++(*counter);
        };
        consider(QStringLiteral("layman"), w.layman, rec->layman, &laymanFixed);
        consider(QStringLiteral("source"), w.source, rec->source, &sourceFixed);

        if (!rec->lanes.isEmpty() && rec->lanes != w.lanes) {
            if (rlIsStrictExtension(w.lanes, rec->lanes)) {
                plan.push_back({pk, QStringLiteral("lanes"), rlLanesJson(rec->lanes)});
                ++lanesFixed;
            } else {
                skippedHere = true;
            }
        }

        if (skippedHere) {
            ++skipped;
            skippedIds.append(w.id);
        }
    }

    if (!dryRun && !plan.isEmpty()) {
        if (!store.begin(&err))
            return rpErr(QStringLiteral("store_failed"), err);
        for (const RlRepairWrite &wr : plan) {
            // `asserted`, not `store-generated`: the recovered text is the
            // author's own adjacent prose and the guard only ever extends an
            // already-asserted value, so this recovers an assertion rather than
            // inventing one. `store-generated` (roadmap-data-model.md § 7.7) is
            // for a field the STORE populates, which layman/source/lanes are
            // not — and every one of these columns already carries `asserted`
            // from the migration that wrote it.
            if (!store.setItemField(wr.pk, wr.field, wr.value,
                                    QStringLiteral("asserted"), &err)) {
                store.rollback(nullptr);
                return rpErr(QStringLiteral("store_failed"), err);
            }
        }
        if (!store.commit(&err)) {
            store.rollback(nullptr);
            return rpErr(QStringLiteral("store_failed"), err);
        }
    }

    QJsonObject env;
    env[QStringLiteral("ok")]               = true;
    env[QStringLiteral("op")]               = QStringLiteral("repair_trailers");
    env[QStringLiteral("project_root")]     = root;
    env[QStringLiteral("items")]            = scanned;
    env[QStringLiteral("items_with_run")]   = withRun;
    env[QStringLiteral("repaired")]         = int(plan.size());
    env[QStringLiteral("layman_repaired")]  = laymanFixed;
    env[QStringLiteral("source_repaired")]  = sourceFixed;
    env[QStringLiteral("lanes_repaired")]   = lanesFixed;
    env[QStringLiteral("chars_recovered")]  = double(charsRecovered);
    env[QStringLiteral("skipped")]          = skipped;
    if (dryRun) env[QStringLiteral("dry_run")] = true;
    // The ids the guard refused, capped. The COUNT is the whole answer and is
    // never capped; the list is a sample to start from, and a truncated one that
    // did not say so would read as the complete set (ANTS-4501's rule).
    constexpr int kSkipSample = 50;
    QJsonArray sample;
    for (int i = 0; i < skippedIds.size() && i < kSkipSample; ++i)
        sample.append(skippedIds.at(i));
    env[QStringLiteral("skipped_ids")] = sample;
    if (skippedIds.size() > kSkipSample)
        env[QStringLiteral("skipped_truncated")] = true;
    return QJsonDocument(env);
}

// Store-only and m_main-independent, so the seam is the section ops' shape.
QJsonDocument RemoteControl::cmdRoadmapLogRepairTrailersForTest(
        const QJsonObject &req) {
    return cmdRoadmapLogRepairTrailers(req);
}
