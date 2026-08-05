// ANTS-3809 § 2.1 — the write shape. See roadmapwrite.h for the contract.

#include "roadmapwrite.h"

namespace RoadmapWrite {

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
