// ANTS-3809 § 2.1 — the write shape. See roadmapwrite.h for the contract.

#include "roadmapwrite.h"

#include "roadmapparse.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QStandardPaths>
#include <QStringView>

#include <algorithm>
#include <utility>

namespace RoadmapWrite {

namespace {

// ANTS-5256 — see the abort site just before store.commit(). Internal linkage:
// the only writer is setForcePostMutateFailForTest() below, which the header
// declares. Mirrors rcdetail::g_forceCounterCommitFail, the seam this project
// already uses for the same job. Never set outside a test.
bool g_forcePostMutateFail = false;


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
    for (const RoadmapParse::BulletRecord &b : bullets) {
        if (!b.idToken.isEmpty())
            out.append(b.idToken);
        // ANTS-5087 — a pass heading has no leading id slot, so idToken is
        // never set and a hand-added pass block was invisible to the guard.
        // Its id is the one synthesised from the designator, which is also
        // the id the store holds for it.
        else if (b.format == QLatin1String("pass-headings"))
            out.append(b.id);
    }
    return out;
}

// Named in the refusal. Capped because the divergence this guard was written
// for is ~200 ids wide and a refusal is not a report: the count is the size of
// the problem, the names are the entry point into it.
constexpr int kNameCap = 25;

// ANTS-4615 — the content of a line with its STYLING removed: lowercase
// alphanumeric words, joined by single spaces, with the status vocabulary
// dropped. `- OK **LOTTO-0001** Headline.` and `- 📋 [LOTTO-0001] **Headline.**`
// both reduce to `lotto 0001 headline`, which is what makes them recognisable
// as the same line in two dialects. Emoji, brackets, asterisks and pipes are
// all non-alphanumeric and fall out for free.
QString contentKey(QStringView line) {
    static const QSet<QString> kStatusWords = {
        QStringLiteral("ok"),      QStringLiteral("todo"),
        QStringLiteral("wip"),     QStringLiteral("done"),
        QStringLiteral("doing"),   QStringLiteral("deferred"),
        QStringLiteral("planned"), QStringLiteral("considered"),
        QStringLiteral("dropped"),   // ANTS-4977
    };
    QString out, tok;
    const auto flush = [&] {
        if (tok.isEmpty()) return;
        if (!kStatusWords.contains(tok)) {
            if (!out.isEmpty()) out += u' ';
            out += tok;
        }
        tok.clear();
    };
    for (const QChar c : line) {
        if (c.isLetterOrNumber()) tok += c.toLower();
        else flush();
    }
    flush();
    return out;
}

constexpr int kLostTextCap = 20;

// What the file holds that `want` (the store alone) does not reproduce, split
// by whether the line's TEXT survives in some other styling.
struct DriftBreakdown {
    int         total    = 0;
    int         restyled = 0;
    // ANTS-4695 — differs from its render twin only in terminal punctuation.
    int         repunctuated = 0;
    // ANTS-4965 — differs from its render twin only in whitespace.
    int         restructured = 0;
    int         lost     = 0;
    QStringList lostText;
    int         gained   = 0;   // ANTS-5350 — see Drift::gained
    // ANTS-4947 — the files that lost TEXT, so the publish can keep a copy of
    // each. Per file rather than a flag, because a project renders into several
    // and only the ones that actually lose prose are worth keeping.
    QStringList lostFiles;
};

// ANTS-4695 — the line with trailing sentence punctuation and surrounding
// whitespace removed. Two lines equal under this but unequal when trimmed
// differ ONLY in how they end, which is what the Layman: parse does on
// purpose (roadmapparse.cpp drops one trailing period for ANTS-1154 INV-4).
// contentKey() cannot see that difference at all -- it strips every
// non-alphanumeric -- so such a line scored as a dialect restyle, and a
// caller reading `discarded_text_lines: 0` was told their prose was
// untouched while thirty of their sentences were about to change.
QString terminalPunctStripped(QStringView line) {
    QString t = line.trimmed().toString();
    while (!t.isEmpty()) {
        const QChar c = t.back();
        if (c == u'.' || c == u',' || c == u';' || c == u':'
            || c == u'!' || c == u'?')
            t.chop(1);
        else
            break;
    }
    return t;
}

// ANTS-4965 — the line with every whitespace character removed.
QString withoutWhitespace(QStringView line) {
    QString out;
    out.reserve(line.size());
    for (const QChar c : line)
        if (!c.isSpace()) out.append(c);
    return out;
}

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
//
// ANTS-4615 — and split by whether the line's TEXT survives. `total` keeps the
// meaning above exactly; `restyled` and `lost` classify the FILE's own lines
// only, so they do not sum to it.
DriftBreakdown driftLines(const QString &have, const QString &want) {
    DriftBreakdown d;
    QHash<QStringView, int> tally;
    const QList<QStringView> wantLines = QStringView(want).split(u'\n');
    tally.reserve(wantLines.size());
    for (QStringView l : wantLines)
        ++tally[l];

    QList<QStringView> fileOnly;
    for (QStringView l : QStringView(have).split(u'\n')) {
        const auto it = tally.find(l);
        if (it != tally.end() && it.value() > 0)
            --it.value();
        else
            fileOnly.append(l);   // the file holds it; the render will not
    }
    d.total = static_cast<int>(fileOnly.size());
    for (auto it = tally.cbegin(); it != tally.cend(); ++it)
        d.total += it.value();   // the render holds it; the file had lost it

    // Content keys of the lines only the RENDER has. A file-only line whose key
    // is among them is the same text in another dialect — the render is about
    // to restyle it, not delete it. Each match is consumed so two file lines
    // cannot both be excused by one render line.
    // ANTS-4695 — keep the render lines themselves, not just a count, so a
    // match can be compared against the twin that excused it. A count alone
    // cannot tell a dialect restyle from a punctuation edit to the author's
    // own prose, and those are different answers to the question the caller
    // is actually asking.
    QHash<QString, QList<QStringView>> renderKeys;
    for (auto it = tally.cbegin(); it != tally.cend(); ++it)
        for (int n = 0; n < it.value(); ++n)
            renderKeys[contentKey(it.key())].append(it.key());

    for (QStringView l : std::as_const(fileOnly)) {
        const auto k = renderKeys.find(contentKey(l));
        if (k != renderKeys.end() && !k.value().isEmpty()) {
            const QStringView twin = k.value().takeLast();
            // ANTS-4965 — same characters, different spacing: an indent or an
            // aligned column the render is about to flatten. A benign dialect
            // restyle changes characters, so it never lands here.
            if (l != twin && withoutWhitespace(l) == withoutWhitespace(twin))
                ++d.restructured;
            else if (terminalPunctStripped(l) == terminalPunctStripped(twin)
                && l.trimmed() != twin.trimmed())
                ++d.repunctuated;
            else
                ++d.restyled;
            continue;
        }
        // Blank and whitespace-only lines are layout, not text. Counting them
        // as lost prose would bury the one line that matters under the noise
        // this item exists to remove.
        if (l.trimmed().isEmpty()) continue;
        ++d.lost;
        if (d.lostText.size() < kLostTextCap)
            d.lostText.append(l.trimmed().toString());
    }
    // ANTS-5350 — render lines no file line matched: what the render adds.
    for (auto it = renderKeys.cbegin(); it != renderKeys.cend(); ++it)
        for (QStringView r : it.value())
            if (!r.trimmed().isEmpty()) ++d.gained;
    return d;
}

// The whole check, over the files a render would rewrite. A file that does not
// exist yet is one the render is about to CREATE, and creating a file destroys
// nothing — so it is skipped rather than counted as wholly discarded.
DriftBreakdown externalDrift(const QHash<QString, QString> &preImage) {
    DriftBreakdown all;
    for (auto it = preImage.cbegin(); it != preImage.cend(); ++it) {
        QFile f(it.key());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const DriftBreakdown d =
            driftLines(QString::fromUtf8(f.readAll()), it.value());
        all.total        += d.total;
        all.restyled     += d.restyled;
        all.repunctuated += d.repunctuated;
        all.restructured += d.restructured;
        all.lost         += d.lost;
        all.gained       += d.gained;   // ANTS-5350
        if (d.lost > 0)
            all.lostFiles.append(it.key());
        for (const QString &t : d.lostText)
            if (all.lostText.size() < kLostTextCap) all.lostText.append(t);
    }
    return all;
}

// ANTS-4947 — keep the file the publish is about to overwrite.
//
// Only the LOST arm reaches here. A restyled or repunctuated line survives in
// the render in another form, so the store still holds its content; a `lost`
// line is by driftLines()'s own definition text the render reproduces in NO
// styling, which makes the file on disk its only copy. Reporting that in a
// capped twenty-line echo and then destroying it leaves a caller retyping their
// own prose out of a response they may already have dropped.
//
// Written OUTSIDE the project, deliberately. A sibling file would appear in
// `git status` every time this fires, in a repository the caller is typically
// mid-commit on — which is how a safety net becomes something people delete on
// sight. GenericDataLocation is the same root RoadmapStore uses, and this
// library already links only Qt6::Core and Qt6::Sql, so it costs no link edge.
//
// Best-effort: a backup that cannot be written must not fail a write that is
// otherwise correct. The envelope names what was kept, so an empty list says
// nothing was — never that nothing was at stake, which the caller reads from
// `discarded_text_lines` as before.
// ANTS-5087 — how many discarded-text backups the directory keeps. Nothing
// removed one before this, so it grew for the life of the install, on the home
// drive, holding copies of files the user never asked to keep.
//
// A COUNT and not an age: it needs no clock, and it gives "how far back can I
// recover?" an answer that does not depend on when you ask. The directory is
// machine-global, like the store, so the cap is shared across projects.
constexpr int kDiscardedFileCap = 200;

// Oldest first, by modification time — QFile::copy stamps the copy with the
// time it was made, so mtime IS when this backup was taken.
//
// Called AFTER the copies, never before: a write must not delete a backup to
// make room for one that then fails to copy. Best-effort throughout, for the
// same reason keepDiscarded is — a prune that cannot run must not fail a write
// that is otherwise correct.
void pruneDiscarded(const QString &dir) {
    QDir d(dir);
    // QDir::Time sorts NEWEST first, so everything at or past the cap is the
    // tail to drop.
    const QFileInfoList files =
        d.entryInfoList({QStringLiteral("*.bak")}, QDir::Files, QDir::Time);
    for (int i = kDiscardedFileCap; i < files.size(); ++i)
        QFile::remove(files.at(i).absoluteFilePath());
}

QStringList keepDiscarded(const QStringList &paths) {
    QStringList kept;
    if (paths.isEmpty())
        return kept;
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty())
        return kept;
    const QString dir = base + QStringLiteral("/ants-terminal/discarded");
    if (!QDir().mkpath(dir))
        return kept;
    // One stamp for the whole publish, so the files of a single write sort
    // together rather than straddling a second boundary.
    const QString stamp =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz"));
    for (const QString &path : paths) {
        // The basename alone collides across projects — every one of them calls
        // it ROADMAP.md — so the absolute path is hashed in. Short: this
        // disambiguates, it does not authenticate.
        const QString tag = QString::fromLatin1(
            QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256)
                .toHex()
                .left(8));
        const QString out = QStringLiteral("%1/%2.%3.%4.bak")
                                .arg(dir, QFileInfo(path).fileName(), tag, stamp);
        if (QFile::exists(out))
            QFile::remove(out);
        if (QFile::copy(path, out))
            kept.append(out);
    }
    pruneDiscarded(dir);
    return kept;
}

}  // namespace

void setForcePostMutateFailForTest(bool on) { g_forcePostMutateFail = on; }


// ANTS-4803 — publish in the dialect the project was MIGRATED FROM, read from
// the store rather than declared by the caller. A caller that chose would be a
// second place for the answer to be wrong, and publishing a pass-headings
// project in bullet form would rewrite the whole file into a dialect its own
// reader does not recognise. '' is a pre-bump row and means ants-v1, which is
// what an empty dialect selects in the render.
// ANTS-5087 — nullopt means the LOOKUP FAILED, which is not the same as "no
// dialect recorded" and must not be published as one. The error was discarded
// here and a failure returned an empty string, which the render reads as
// ants-v1: a transient SQL error while reading this one row would have
// rewritten a pass-headings roadmap into bullet form, the exact loss the
// comment above says the store-side lookup exists to prevent. An absent ROW
// still means ants-v1, unchanged — that is the pre-bump case.
std::optional<QString> dialectOf(RoadmapStore &store, qint64 projectId) {
    QString err;
    const auto row = store.readProject(projectId, &err);
    if (row)
        return row->sourceFormat;
    // The readers here set `error` when the QUERY failed and leave it untouched
    // when the query ran and matched nothing.
    if (!err.isEmpty())
        return std::nullopt;
    return QString();
}

// ANTS-4462 — the read half. Deliberately the SAME pre-image render and the
// SAME externalDrift() the write path runs at step 0, so "is my file in sync?"
// and "what would this write overwrite?" can never answer differently. A second
// implementation would be a second answer.
//
// No transaction: render() only reads, and wrapping it would make a read-only
// query contend with writers on the machine-global store.
std::optional<Drift> measureDrift(RoadmapStore &store, qint64 projectId,
                                  const QString &projectRoot,
                                  const QString &liveRoadmapPath) {
    // ANTS-5087 — a dialect the store could not be asked for is no answer, and
    // this check would otherwise report drift computed against the wrong
    // emission. Nothing to report is the honest result.
    const auto dialect = dialectOf(store, projectId);
    if (!dialect)
        return std::nullopt;

    RoadmapRender::Options pre;
    pre.liveRoadmapPath = liveRoadmapPath;
    pre.dialect = *dialect;
    pre.dryRun = true;
    // ANTS-4628 — an ENGAGED EMPTY scope, judging nothing. A staleness check
    // that failed the Layman gate would go dark on exactly the projects
    // carrying legacy debt, which is where the question is worth asking.
    pre.gateScope = QSet<qint64>{};
    QHash<QString, QString> preImage;
    if (!RoadmapRender::render(store, projectId, projectRoot, pre,
                               nullptr, &preImage)
        || preImage.isEmpty())
        return std::nullopt;
    const DriftBreakdown d = externalDrift(preImage);
    Drift out;
    out.total        = d.total;
    out.restyled     = d.restyled;
    out.repunctuated = d.repunctuated;
    out.restructured = d.restructured;
    out.lost         = d.lost;
    out.gained       = d.gained;   // ANTS-5350
    out.lostText = d.lostText;
    return out;
}

Result commitAndRender(RoadmapStore &store, qint64 projectId,
                       const QString &projectRoot,
                       const QString &liveRoadmapPath, bool dryRun,
                       const std::function<bool(QString *)> &mutate,
                       RoadmapRender::Outcome *outcome,
                       QString *error, LaymanGate gate) {
    if (error)
        error->clear();

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

    // Step 0 — ANTS-4462 / ANTS-4465, the PRE-IMAGE. Render the store as it
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
    DriftBreakdown drift;
    bool driftChecked = false;
    if (const auto preDialect = dialectOf(store, projectId)) {
        // ANTS-5087 — and skipped outright when the dialect could not be read.
        // `externalEditsChecked:false` already means "nobody looked", which is
        // the truth here; measuring against the wrong emission would report
        // every line of the file as drift.
        RoadmapRender::Options pre;
        pre.liveRoadmapPath = liveRoadmapPath;
        pre.dialect = *preDialect;
        pre.dryRun = true;
        // ANTS-4628 — an ENGAGED EMPTY scope, so this diagnostic render judges
        // nothing. It has to: the pre-image exists only to measure drift, and
        // an unset scope would gate it on the whole project — so on exactly the
        // projects carrying legacy debt the pre-image came back empty, the
        // drift check silently did not run, and `externalEditsChecked` reported
        // false. The measurement was unavailable precisely where it mattered.
        pre.gateScope = QSet<qint64>{};
        QHash<QString, QString> preImage;
        if (RoadmapRender::render(store, projectId, projectRoot, pre, nullptr, &preImage)
            && !preImage.isEmpty()) {
            drift = externalDrift(preImage);
            driftChecked = true;
        }
    }

    // Step 1 — ANTS-5087. BEGIN IMMEDIATE opens HERE, after the pre-image, so
    // the write transaction spans the validating render only, which is what
    // ANTS-3809 § 4 says it spans. The pre-image reads the store as it stands
    // and the mutation has not run, so the measurement is identical on either
    // side of this line; all that changes is how long the lock is held. It was
    // held across two render walks and externalDrift()'s file reparse.
    if (!store.begin(error))
        return Result::StoreFailed;

    // ANTS-4947 — filled only once the commit has succeeded and the publish is
    // about to run, so a dry run and every aborted write leave it empty. That is
    // the ANTS-4463 tense rule applied to a file: nothing was overwritten, so
    // nothing was kept.
    QStringList keptBackups;

    // ANTS-4844 — filled in the one window where the mutated rows exist; see
    // RoadmapRender::Outcome::touchedBullets.
    QMap<QString, QString> touchedBullets;

    // Every Outcome that leaves this function carries the measurement, so the
    // envelope sees it whichever render produced the rest of the fields.
    const auto publish = [&](const RoadmapRender::Outcome &o) {
        if (!outcome)
            return;
        *outcome = o;
        outcome->externalEditsChecked     = driftChecked;
        outcome->externalEditLines        = drift.total;
        outcome->externalRestyledLines    = drift.restyled;
        outcome->externalRepunctuatedLines = drift.repunctuated;
        outcome->externalRestructuredLines = drift.restructured;
        outcome->externalTextLines        = drift.lost;
        outcome->externalLostText         = drift.lostText;
        outcome->externalLostTextTruncated = drift.lost > drift.lostText.size();
        outcome->externalLostBackups      = keptBackups;
        outcome->touchedBullets           = touchedBullets;   // ANTS-4844
    };

    // Step 2.
    if (!mutate(error))
        return abort(Result::StoreFailed);

    // Steps 3–4 — validate with a DRY render, before the commit. This is the
    // ordering INV-1 is about: render() commits its own files, so validating
    // with the real one would leave files staged behind a store that then
    // rolled back.
    // ANTS-5087 — refuse rather than guess. This is the dialect the file is
    // REWRITTEN in, so a lookup that failed used to fall back to ants-v1 and
    // publish a pass-headings roadmap as bullets — the whole file, into a
    // dialect its own reader does not recognise, with ok:true. The store is
    // rolled back and nothing is written.
    const auto dialect = dialectOf(store, projectId);
    if (!dialect) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("the roadmap store could not be asked which "
                                    "dialect project %1 was migrated from; "
                                    "nothing was written")
                         .arg(projectId);
        }
        return abort(Result::StoreFailed);
    }

    RoadmapRender::Options opts;
    opts.liveRoadmapPath = liveRoadmapPath;
    opts.dialect = *dialect;
    opts.dryRun = true;
    // ANTS-4628 / ANTS-3758 § 2.5 — the Layman gate judges what THIS write
    // touched, taken from the store rather than declared by the caller. Read
    // after mutate() and before the commit, which is the only window in which
    // the set is both complete and still present.
    //
    // Whole-project scoping was withdrawn because it is self-blocking: the gate
    // runs after the mutation, so on a project carrying legacy debt every write
    // was refused by items it never touched — the repairs included, which is
    // ANTS-4434's deadlock. An `op:render` mutates nothing, so this set is
    // empty and it publishes, which is how a project that has never been
    // rendered gets its ids into the file at all (ANTS-4628).
    opts.gateScope = store.itemsWrittenSinceBegin();
    // ANTS-5256 — op:"convert" only. The scope above is correct and stays
    // correct; what the exemption says is that the RULE does not apply to a
    // migration. Offenders come back in Outcome::laymanMissing and the render
    // publishes. See RoadmapRender::Options::laymanGateAdvisory.
    opts.laymanGateAdvisory = (gate == LaymanGate::Exempt);

    // ANTS-4844 — the rendered bullet for each item this write touched, taken
    // in the SAME window and for the same reason the scope above is: after
    // mutate() and before the commit is the only point at which the mutated row
    // exists. A dry run rolls back before this function returns, so an envelope
    // builder rendering the bullet afterwards would render the PRE-write state
    // — a preview that is confidently wrong, which is the defect this fixes
    // rather than reproduces.
    //
    // op:"flip" EDITS an existing bullet where op:"append" adds a new one, so it
    // is the higher-stakes of the two and had the weaker preview: the only way
    // to see what it produced was to run it for real and read the file back,
    // which is what dry_run exists to avoid.
    //
    // Per DIALECT, because the two emit different blocks and rendering an
    // ants-v1 bullet for a pass-headings project would preview text that file
    // will never contain (ANTS-4803).
    //
    // Capped: a flip_batch can touch many items, and an unbounded echo would
    // put the whole project in an envelope. The cap is silent because the
    // per-item bullets are an aid, not an accounting array — `flipped[]`
    // already names every item the batch touched.
    {
        constexpr int kMaxTouchedBullets = 25;
        const bool passHeadings =
            (*dialect == QLatin1String("pass-headings"));
        for (qint64 pk : *opts.gateScope) {
            if (touchedBullets.size() >= kMaxTouchedBullets)
                break;
            const auto row = store.readItem(pk, nullptr);
            if (!row || row->id.isEmpty())
                continue;
            touchedBullets.insert(row->id,
                                  passHeadings ? RoadmapRender::passBlockText(*row)
                                               : RoadmapRender::bulletText(*row));
        }
    }

    const auto dry = RoadmapRender::render(store, projectId, projectRoot, opts, error);
    if (!dry)
        return abort(Result::RenderFailed);
    // Set before the gate check: GateUnmet's envelope is built from
    // `gateFailures`, so the refusal path needs the Outcome as much as the
    // success path does.
    publish(*dry);
    if (!dry->gateFailures.isEmpty()) {
        if (error && error->isEmpty()) {
            // ANTS-4628 — the gate now names only what THIS write touched, so
            // every id in gate_failures is one the caller is already editing
            // and the remedy is always in reach. That was not true under
            // whole-project scoping, where the message had to explain why the
            // obvious per-item repair could not work (ANTS-4434): each single
            // repair was refused by the offenders that remained and rolled
            // back. The batch escape that message prescribed is no longer
            // needed, and the message no longer prescribes it.
            // ANTS-5267 — CAPPED. This joined every id into the prose with no
            // limit, which was right while the gate judged only the items a
            // write TOUCHED: the caller is editing them and naming them all is
            // the remedy. A whole-project write broke that assumption —
            // measured on Vestige, 460 ids, ~4 KB of prose then TRUNCATED
            // MID-LIST by the transport with 1,947 characters cut. The caller
            // got neither the full list nor a usable summary, and no marker
            // saying which.
            //
            // The structured `gate_failures` array carries every id regardless,
            // so the prose never needed to. Same treatment as ANTS-5256's
            // `layman_missing`: cap, state the true total, announce the cut.
            const QStringList namedGate = dry->gateFailures.mid(0, kNameCap);
            *error = QStringLiteral("the roadmap render refuses this write: %1 open "
                                    "item(s) it touches carry no Layman: line (%2%3). Give "
                                    "each one a one-sentence summary — a note sets the column "
                                    "when `Layman:` is first on its own line, whichever line of "
                                    "the note that is, so it can ride along in this same call. "
                                    "Only items this "
                                    "write touches are judged; the project's other items "
                                    "are not, and cannot block it.")
                         .arg(dry->gateFailures.size())
                         .arg(namedGate.join(QStringLiteral(", ")),
                              dry->gateFailures.size() > namedGate.size()
                                  ? QStringLiteral(", +%1 more — the full list is "
                                                   "in `gate_failures`")
                                        .arg(dry->gateFailures.size() - namedGate.size())
                                  : QString());
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
        // ANTS-5016 — an unchanged file is still a published file whose ids
        // count, so the guard reads both halves and its coverage is unchanged.
        const QStringList owned = dry->filesWritten + dry->filesUnchanged;
        for (const QString &path : owned) {
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

    // ANTS-5256 — test-only, and the last statement before the point of no
    // return, which is the whole point: everything above it really ran, so an
    // abort here exercises the rollback of a COMPLETED mutation.
    //
    // It exists because the two invariants that need this window (convert's
    // INV-2 inert-on-failure and INV-4 ids-stable-across-commit) used to force
    // their failure with the render's Layman gate, and this item exempted
    // convert from that gate — so both cases would have gone green by losing
    // their trigger rather than by holding. Borrowing whichever business rule
    // happens to refuse today is what made them fragile; a seam named for the
    // window cannot be invalidated by a rule change.
    if (g_forcePostMutateFail) {
        if (error && error->isEmpty())
            *error = QStringLiteral("forced post-mutate failure (test seam)");
        return abort(Result::StoreFailed);
    }

    // Step 6.
    if (!store.commit(error))
        return abort(Result::StoreFailed);

    // ANTS-4947 — the last moment the overwritten text still exists. After the
    // render below, the only copy of a `lost` line is whatever was kept here.
    keptBackups = keepDiscarded(drift.lostFiles);

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
