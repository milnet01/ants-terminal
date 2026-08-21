// ANTS-3809 § 2.1 — the write shape. See roadmapwrite.h for the contract.

#include "roadmapwrite.h"

#include "roadmapparse.h"

#include <QFile>
#include <QHash>
#include <QSet>
#include <QStringView>

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
    // ANTS-3771 — deliberately the BARE overload, and INV-13's grep should
    // read this comment rather than flag it. Two reasons, and the second is
    // decisive. This function reads a file that commitAndRender() is about to
    // overwrite, which means the project is MIGRATED — so the file it reads is
    // roadmaprender.cpp's own ants-v1 output, `gfmHere` is false, and the
    // branch a declaration governs cannot run (the same argument INV-6 makes
    // for the store path). And this library links
    // `Qt6::Core Qt6::Sql ants_roadmapparse_lib` and deliberately not
    // ants_core_lib, so ProjectSettings is not reachable from here at all;
    // threading a value down would add a parameter to commitAndRender() and
    // ten call sites to carry something that cannot change the answer.
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

// ANTS-4462 / ANTS-4465 — how far `have` (the file on disk) has drifted from
// `want` (what the store alone renders), counted in lines.
//
// A MULTISET of lines, not a diff. Two reasons, and neither is about saving
// effort. An LCS over a 20k-line file is O(n·m) and this runs on every write.
// And the answer a caller needs is "how much text is at stake", which a
// multiset gives exactly: a line the render MOVED is present on both sides and
// scores nothing, so a reflow does not read as data loss, while a line only the
// file has (a hand-edit about to be overwritten) and a line only the render has
// (a hand-DELETION about to be undone) each score one. Both directions count,
// because a deletion silently reverted is the same class of surprise as an
// insertion silently dropped, and reporting only one arm would hand a session
// that hand-deleted a stale line a clean bill of health.
int driftLines(const QString &have, const QString &want) {
    QHash<QStringView, int> tally;
    const QList<QStringView> wantLines = QStringView(want).split(u'\n');
    tally.reserve(wantLines.size());
    for (QStringView l : wantLines)
        ++tally[l];

    int drift = 0;
    for (QStringView l : QStringView(have).split(u'\n')) {
        const auto it = tally.find(l);
        if (it != tally.end() && it.value() > 0)
            --it.value();
        else
            ++drift;   // the file holds it; the render will not reproduce it
    }
    for (auto it = tally.cbegin(); it != tally.cend(); ++it)
        drift += it.value();   // the render holds it; the file had lost it
    return drift;
}

// The whole check, over the files a render would rewrite. A file that does not
// exist yet is one the render is about to CREATE, and creating a file destroys
// nothing — so it is skipped rather than counted as wholly discarded.
int externalDrift(const QHash<QString, QString> &preImage) {
    int drift = 0;
    for (auto it = preImage.cbegin(); it != preImage.cend(); ++it) {
        QFile f(it.key());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        drift += driftLines(QString::fromUtf8(f.readAll()), it.value());
    }
    return drift;
}

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

    // Step 1b — ANTS-4462 / ANTS-4465, the PRE-IMAGE. Render the store as it
    // stands, before the mutation touches it, and measure how far the file on
    // disk has drifted from it.
    //
    // Why a second render and not a diff of the one below: the step-3 render
    // runs AFTER the mutation, so its text differs from the file by the change
    // this call was made to write. Diffing that would flag every healthy write.
    // The pre-image differs from the file by nothing BUT what arrived from
    // outside the store — a hand-edit (ANTS-4465) or a store that fell behind
    // the file (ANTS-4462's data-loss half) — which is the one question worth
    // asking a moment before the publish overwrites it.
    //
    // Nor is raw mtime a substitute: this sequence writes the store and THEN
    // the file, so after every healthy write the file is the newer of the two
    // and an mtime test reports stale on every project.
    //
    // Diagnostic only. A failure here is swallowed — `error` is deliberately
    // not passed, so a pre-image that cannot be built leaves the caller's own
    // error channel clean — and the write proceeds with
    // `externalEditsChecked:false`, which reads as "nobody looked" rather than
    // as "clean". Refusing on drift was considered and rejected: it is the
    // render_gate_unmet shape, where one hand-edit anywhere bricks every op on
    // the project, and these two items ask to be TOLD, not blocked.
    int drift = 0;
    bool driftChecked = false;
    {
        RoadmapRender::Options pre;
        pre.liveRoadmapPath = liveRoadmapPath;
        pre.dryRun = true;
        QHash<QString, QString> preImage;
        if (RoadmapRender::render(store, projectId, projectRoot, pre, nullptr, &preImage)
            && !preImage.isEmpty()) {
            drift = externalDrift(preImage);
            driftChecked = true;
        }
    }

    // Every Outcome that leaves this function carries the measurement, so the
    // envelope sees it whichever render produced the rest of the fields.
    const auto publish = [&](const RoadmapRender::Outcome &o) {
        if (!outcome)
            return;
        *outcome = o;
        outcome->externalEditsChecked = driftChecked;
        outcome->externalEditLines = drift;
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
    publish(*dry);
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
    if (published)
        publish(*published);
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
