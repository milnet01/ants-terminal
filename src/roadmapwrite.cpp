// ANTS-3809 § 2.1 — the write shape. See roadmapwrite.h for the contract.

#include "roadmapwrite.h"

#include "roadmapparse.h"

#include <QFile>
#include <QSet>

#include <algorithm>

namespace RoadmapWrite {
namespace {

// ANTS-4141 — the ids one file carries, from each bullet's own leading
// `[<PREFIX>-NNNN]` slot.
//
// `idToken` and NOT `id`: BulletRecord::id is positionless, taking the first id
// token found ANYWHERE in the body (roadmapparse.h), so an id-less bullet that
// merely MENTIONS an id would be read as owning it and the guard would refuse a
// render that drops nothing. A file that does not exist yet is one the render
// is about to create, and creating a file drops nothing.
QStringList fileIds(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QStringList out;
    const auto bullets = RoadmapParse::parseBullets(QString::fromUtf8(f.readAll()));
    out.reserve(bullets.size());
    for (const RoadmapParse::BulletRecord &b : bullets)
        if (!b.idToken.isEmpty())
            out.append(b.idToken);
    return out;
}

// Named in the refusal. Capped because the divergence this guard was written
// for is ~200 ids wide and a refusal is not a report: the count is the size of
// the problem, the names are the entry point into it.
constexpr int kNameCap = 25;

}  // namespace

Result commitAndRender(RoadmapStore &store, qint64 projectId,
                       const QString &projectRoot,
                       const QString &liveRoadmapPath, bool dryRun,
                       const std::function<bool(QString *)> &mutate,
                       RoadmapRender::Outcome *outcome,
                       QString *error) {
    if (error)
        error->clear();

    // Step 1.
    if (!store.begin(error))
        return Result::StoreFailed;

    // A rollback that itself refuses is folded into the caller's failure rather
    // than reported. SQLite leaves the transaction open when a COMMIT fails, so
    // the rollback is the right call and normally succeeds; rollback() "refuses
    // when none is open", so the one case where it does not is the one where
    // there is nothing left to roll back. Reporting a second failure there
    // would tell the caller about the recovery instead of about the fault —
    // hence the nullptr, which also keeps *error holding the real fault.
    const auto abort = [&store](Result r) {
        store.rollback();
        return r;
    };

    // Step 2.
    if (!mutate(error))
        return abort(Result::StoreFailed);

    // Steps 3–4 — validate with a DRY render, before the commit. This is the
    // ordering INV-1 is about: render() commits its own files, so validating
    // with the real one would leave files staged behind a store that then
    // rolled back.
    RoadmapRender::Options opts;
    opts.liveRoadmapPath = liveRoadmapPath;
    opts.dryRun = true;
    const auto dry = RoadmapRender::render(store, projectId, projectRoot, opts, error);
    if (!dry)
        return abort(Result::RenderFailed);
    // Set before the gate check: GateUnmet's envelope is built from
    // `gateFailures`, so the refusal path needs the Outcome as much as the
    // success path does.
    if (outcome)
        *outcome = *dry;
    if (!dry->gateFailures.isEmpty()) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("the roadmap render refuses this project: %1 open "
                                    "item(s) carry no Layman: line")
                         .arg(dry->gateFailures.size());
        }
        return abort(Result::GateUnmet);
    }

    // Step 4b — ANTS-4141's divergence guard. See the header for why this sits
    // here and what it deliberately does not cover. Before `dryRun` returns, so
    // a preview reports the refusal a real call would hit, as the gate above
    // already does.
    {
        const QSet<QString> rendered(dry->renderedIds.cbegin(), dry->renderedIds.cend());
        QStringList dropped;
        QSet<QString> seen;
        for (const QString &path : std::as_const(dry->filesWritten)) {
            for (const QString &id : fileIds(path)) {
                if (rendered.contains(id) || seen.contains(id))
                    continue;
                seen.insert(id);
                dropped.append(id);
            }
        }
        if (!dropped.isEmpty()) {
            std::sort(dropped.begin(), dropped.end());
            if (error) {
                const QStringList named = dropped.mid(0, kNameCap);
                *error = QStringLiteral(
                             "the roadmap render would DELETE %1 bullet(s) the store "
                             "has never imported: %2%3. Nothing was written and the "
                             "store is rolled back. Import them (roadmap_migrate) "
                             "before writing through this verb again; until then, "
                             "edit the roadmap by hand.")
                             .arg(dropped.size())
                             .arg(named.join(QStringLiteral(", ")),
                                  dropped.size() > named.size()
                                      ? QStringLiteral(", +%1 more")
                                            .arg(dropped.size() - named.size())
                                      : QString());
            }
            return abort(Result::WouldDrop);
        }
    }

    // Step 5 — the caller wanted a preview. Everything above ran, so *outcome
    // carries the whole would-be result; nothing is committed on either the
    // store or the file (INV-7).
    if (dryRun)
        return abort(Result::Ok);

    // Step 6.
    if (!store.commit(error))
        return abort(Result::StoreFailed);

    // Steps 7–8 — publish. The store is committed from here on and STAYS so on
    // failure: see the header for why leaving the file stale-behind is the
    // deliberate answer rather than a gap.
    opts.dryRun = false;
    const auto published =
        RoadmapRender::render(store, projectId, projectRoot, opts, error);
    if (published && outcome)
        *outcome = *published;
    if (!published || !published->committed) {
        if (error && error->isEmpty()) {
            *error = published
                ? QStringLiteral("the roadmap store committed, but the render wrote "
                                 "only %1 of its files; re-run the render")
                      .arg(published->filesWritten.size())
                : QStringLiteral("the roadmap store committed, but the render did "
                                 "not run; re-run the render");
        }
        return Result::PublishFailed;
    }
    return Result::Ok;
}

} // namespace RoadmapWrite
