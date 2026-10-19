// ANTS-5315 step 4 — roadmap_log op:"place_by_release": file each shipped
// item under the release it shipped in.
// Contract: docs/specs/ANTS-5315-release-sections-migration.md.
//
// Its own TU, beside remotecontrol_roadmap_backfill.cpp, because it is the
// same kind of member: a one-off, store-only migration op that reads git,
// plans every write first, and writes them in one commitAndRender().
#include "remotecontrol.h"
#include "remotecontrol_internal.h"
#include "changelogquery.h"
#include "gitwrap.h"
#include "hostexec.h"      // ANTS-5527 — tools run on the host inside a Flatpak
#include "roadmapfoldin.h"
#include "roadmapindex.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <limits>
#include <tuple>

using namespace rcdetail;

namespace {

using RlVerKey = std::tuple<qint64, qint64, qint64>;

// § 2.2 — a release version is exactly X.Y.Z; a `v` is allowed on a tag only.
std::optional<RlVerKey> rlStableKey(const QString &v) {
    static const QRegularExpression rx(QStringLiteral("^(\\d+)\\.(\\d+)\\.(\\d+)$"));
    const auto m = rx.match(v);
    if (!m.hasMatch()) return std::nullopt;
    return RlVerKey{m.captured(1).toLongLong(), m.captured(2).toLongLong(),
                    m.captured(3).toLongLong()};
}

bool rlVersionLess(const QString &a, const QString &b) {
    return rlStableKey(a).value_or(RlVerKey{}) < rlStableKey(b).value_or(RlVerKey{});
}

// ANTS-1878 § 2.3a's ordering key: a patch stream `x` sorts after every
// numbered patch of its minor. The same lambda create_section uses.
RlVerKey rlPlacementKey(const QString &v) {
    const QStringList p = v.split(QLatin1Char('.'));
    const qint64 patch = p.value(2) == QLatin1String("x")
        ? std::numeric_limits<int>::max() : p.value(2).toLongLong();
    return {p.value(0).toLongLong(), p.value(1).toLongLong(), patch};
}

// § 2.2 — the id a commit subject BEGINS with, followed by a non-word
// character or the end of the subject. Empty when the subject leads with none.
QString rlSubjectId(const QString &subject) {
    static const QRegularExpression rx(
        QStringLiteral("^([A-Za-z][A-Za-z0-9_]*-\\d+)(?=\\W|$)"));
    const auto m = rx.match(subject);
    return m.hasMatch() ? m.captured(1) : QString();
}

// § 2.3 — `0.7.50–0.7.59 — …`: a version, a dash, then another version.
bool rlIsRangeTitle(const QString &title) {
    static const QRegularExpression rx(QStringLiteral(
        "^\\s*v?\\d+\\.\\d+\\.\\d+\\s*[\\x{2013}\\x{2014}-]\\s*v?\\d+\\.\\d+"));
    return rx.match(title).hasMatch();
}

// § 2.3's retitle: the leading version text (everything before the first
// ` — `) moves to the end as ` (<text> wave)`.
QString rlWaveTitle(const QString &title) {
    const QString sep = QStringLiteral(" — ");
    const int at = title.indexOf(sep);
    if (at < 0)
        return QStringLiteral("Release wave (%1)").arg(title.trimmed());
    return QStringLiteral("%1 (%2 wave)")
        .arg(title.mid(at + sep.size()).trimmed(), title.left(at).trimmed());
}

struct RlGitOut {
    QByteArray out;
    bool failed = false;
    QString error;
};

// One git call under the op's shared wall budget (§ 2.5).
RlGitOut rlRunGit(const QString &root, const QStringList &args,
                  const QElapsedTimer &clock, int budgetMs) {
    RlGitOut r;
    QProcess p;
    p.setWorkingDirectory(root);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    HostExec::start(p, QStringLiteral("git"), args);
    if (!p.waitForStarted(2000)) {
        r.failed = true;
        r.error  = QStringLiteral("git failed to start");
        return r;
    }
    while (true) {
        if (clock.elapsed() > budgetMs) {
            p.kill();
            p.waitForFinished(500);
            r.failed = true;
            r.error  = QStringLiteral("git exceeded the %1 s budget").arg(budgetMs / 1000);
            return r;
        }
        if (p.waitForReadyRead(200)) {
            r.out += p.readAllStandardOutput();
            continue;
        }
        if (p.state() == QProcess::NotRunning) {
            r.out += p.readAllStandardOutput();
            break;
        }
    }
    p.waitForFinished(1000);
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        r.failed = true;
        r.error  = QStringLiteral("git %1 exited %2: %3")
                       .arg(args.value(0)).arg(p.exitCode())
                       .arg(QString::fromUtf8(p.readAllStandardError().left(400)).trimmed());
    }
    return r;
}

// § 2.2 item 2 — the tag source.
struct RlTagEvidence {
    QHash<QString, QString> versionOf;   // id -> lowest stable tag containing its subject commit
    QSet<QString> untagged;              // ids whose subject commit no stable tag contains
    QHash<QString, QString> tagDate;     // version -> the tag's creator date
    int stableTags = 0;
    bool failed = false;
    QString error;
};

RlTagEvidence rlReadTags(const QString &root, int budgetMs) {
    RlTagEvidence ev;
    // § 4 — not a git repository: the source gives nothing, and that is no error.
    const GitWrap::Result probe =
        GitWrap::run(root, {QStringLiteral("rev-parse"), QStringLiteral("--git-dir")});
    if (!probe.started || probe.exitCode != 0) return ev;

    QElapsedTimer clock;
    clock.start();
    const auto fail = [&ev](const RlGitOut &g) {
        ev.failed = true;
        ev.error  = g.error;
        return ev;
    };
    const RlGitOut refs = rlRunGit(root,
        {QStringLiteral("for-each-ref"),
         QStringLiteral("--format=%(refname:short)%09%(creatordate:short)"),
         QStringLiteral("refs/tags")}, clock, budgetMs);
    if (refs.failed) return fail(refs);

    // A tag with any suffix (`v1.0.0-rc1`) is not a release (INV-11).
    static const QRegularExpression stable(QStringLiteral("^v?(\\d+\\.\\d+\\.\\d+)$"));
    struct Tag { QString name, version, date; };
    QVector<Tag> tags;
    QSet<QString> seen;
    for (const QByteArray &raw : refs.out.split('\n')) {
        const QStringList cols = QString::fromUtf8(raw).split(QLatin1Char('\t'));
        const auto m = stable.match(cols.value(0));
        if (!m.hasMatch() || seen.contains(m.captured(1))) continue;
        seen.insert(m.captured(1));
        tags.push_back({cols.value(0), m.captured(1), cols.value(1).trimmed()});
    }
    std::sort(tags.begin(), tags.end(), [](const Tag &a, const Tag &b) {
        return rlVersionLess(a.version, b.version);
    });
    ev.stableTags = int(tags.size());

    const auto subjectsInto = [&](const QByteArray &out, const auto &onId) {
        for (const QByteArray &line : out.split('\n')) {
            const QString id = rlSubjectId(QString::fromUtf8(line));
            if (!id.isEmpty()) onId(id);
        }
    };
    // Lowest first: the commits a tag holds that no lower tag holds.
    QStringList lower;
    for (const Tag &t : std::as_const(tags)) {
        QStringList args{QStringLiteral("log"), QStringLiteral("--format=%s"),
                         QStringLiteral("refs/tags/") + t.name};
        if (!lower.isEmpty()) args << QStringLiteral("--not") << lower;
        args << QStringLiteral("--");
        const RlGitOut g = rlRunGit(root, args, clock, budgetMs);
        if (g.failed) return fail(g);
        subjectsInto(g.out, [&](const QString &id) {
            if (!ev.versionOf.contains(id)) ev.versionOf.insert(id, t.version);
        });
        ev.tagDate.insert(t.version, t.date);
        lower << QStringLiteral("refs/tags/") + t.name;
    }

    // Commits on any ref that no stable tag contains yet: `unreleased`
    // evidence. Every ref and not HEAD, because a release candidate's tag
    // usually sits on a branch of its own.
    {
        QStringList args{QStringLiteral("log"), QStringLiteral("--format=%s"),
                         QStringLiteral("--all")};
        if (!lower.isEmpty()) args << QStringLiteral("--not") << lower;
        args << QStringLiteral("--");
        const RlGitOut g = rlRunGit(root, args, clock, budgetMs);
        if (g.failed) return fail(g);
        subjectsInto(g.out, [&](const QString &id) { ev.untagged.insert(id); });
    }
    return ev;
}

// § 2.2 item 1 — the CHANGELOG source.
struct RlChangelogEvidence {
    QString relPath;                          // empty: no changelog
    QHash<QString, QStringList> cited;        // id -> released versions citing it
    QSet<QString> unreleased;                 // ids cited under [Unreleased]
    QSet<QString> versions;                   // every `## [X.Y.Z]` heading
    QHash<QString, QString> headingDate;      // version -> ISO date, when ISO
};

RlChangelogEvidence rlReadChangelog(const QString &root, const QString &roadmapPath,
                                    const QString &prefixFallback) {
    RlChangelogEvidence ev;
    const QString path = findChangelogUnder(root);
    if (path.isEmpty()) return ev;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return ev;
    ev.relPath = QDir(root).relativeFilePath(path);
    const QString prefix = RoadmapFoldIn::sniffIdPrefix(
        QFileInfo(roadmapPath).absolutePath(), prefixFallback);
    const ChangelogQuery::ParseResult parsed =
        ChangelogQuery::parse(QString::fromUtf8(f.readAll()), prefix);
    static const QRegularExpression iso(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}$"));
    for (const ChangelogQuery::VersionInfo &v : parsed.versions) {
        if (v.unreleased || !rlStableKey(v.version)) continue;
        ev.versions.insert(v.version);
        if (iso.match(v.date.trimmed()).hasMatch())
            ev.headingDate.insert(v.version, v.date.trimmed());
    }
    for (const ChangelogQuery::Entry &e : parsed.entries) {
        for (const QString &id : e.ids) {
            if (e.unreleased) {
                ev.unreleased.insert(id);
            } else if (rlStableKey(e.version)) {
                QStringList &vs = ev.cited[id];
                if (!vs.contains(e.version)) vs << e.version;
            }
        }
    }
    return ev;
}

}  // namespace

QJsonDocument RemoteControl::cmdRoadmapLogPlaceByRelease(const QJsonObject &req) {
    const auto pbErr = [](const QString &code, const QString &message,
                          const QString &hint = QString()) {
        QJsonObject env;
        env[QStringLiteral("ok")]    = false;
        env[QStringLiteral("code")]  = code;
        env[QStringLiteral("error")] = message;
        if (!hint.isEmpty()) env[QStringLiteral("hint")] = hint;
        return QJsonDocument(env);
    };

    // § 2.1 — dry run first is enforced. Only the wrapper's plan_id re-dispatch
    // carries the mark, and the wrapper strips it from every direct request.
    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();
    if (!dryRun && !req.value(QLatin1String(kPlanApplyKey)).toBool())
        return pbErr(QStringLiteral("bad_args"),
            QStringLiteral("roadmap_log: place_by_release moves most of a "
                           "project's items in one call, so it applies only a "
                           "plan from its own dry run"),
            QStringLiteral("Send dry_run:true, read the plan, then send its "
                           "plan_id to apply it."));
    int sampleLimit = 50;
    if (req.contains(QStringLiteral("sample_limit"))) {
        const QJsonValue v = req.value(QStringLiteral("sample_limit"));
        const double d = v.toDouble(-1);
        if (!v.isDouble() || d != double(int(d)) || d < 1 || d > 200)
            return pbErr(QStringLiteral("bad_args"),
                QStringLiteral("roadmap_log: sample_limit is a whole number "
                               "from 1 to 200"));
        sampleLimit = int(d);
    }

    QString root, roadmapPath;
    QJsonDocument refusal;
    const auto target = roadmapSectionOpTarget(req, &root, &roadmapPath, &refusal);
    if (!target) {
        QJsonObject env = refusal.object();
        if (env.value(QStringLiteral("code")).toString() ==
                QLatin1String("op_unsupported")) {
            QString storeErr;
            env = rlNotStoreServedRefusal(env, roadmapStoreOrNull(nullptr, &storeErr),
                                          root, roadmapPath,
                                          QStringLiteral("place_by_release needs a store-served project"),
                                          QStringLiteral("Run roadmap_migrate first."));
            return QJsonDocument(env);
        }
        return refusal;
    }
    RoadmapStore &store    = *target->store;
    const qint64 projectId = target->projectId;

    QString err;
    const auto sectionsOpt = store.listSectionsOrdered(projectId, &err);
    if (!sectionsOpt) return pbErr(QStringLiteral("store_failed"), err);
    const QVector<RoadmapStore::SectionRow> &secs = *sectionsOpt;
    const auto itemsOpt = store.readItems(projectId, &err);
    if (!itemsOpt) return pbErr(QStringLiteral("store_failed"), err);
    const QHash<qint64, RoadmapStore::ItemWrite> &items = *itemsOpt;
    const auto historyOpt = store.placementHistory(projectId, &err);
    if (!historyOpt) return pbErr(QStringLiteral("store_failed"), err);

    // The fallback prefix for the CHANGELOG's id extraction: the store's most
    // common one, used only when ROADMAP.md names none.
    QString prefixFallback;
    {
        QHash<QString, int> n;
        for (const auto &it : items) {
            const int dash = it.id.lastIndexOf(QLatin1Char('-'));
            if (dash > 0) ++n[it.id.left(dash)];
        }
        int best = 0;
        for (auto it = n.cbegin(); it != n.cend(); ++it)
            if (it.value() > best) { best = it.value(); prefixFallback = it.key(); }
    }
    const RlChangelogEvidence cl = rlReadChangelog(root, roadmapPath, prefixFallback);
    // ANTS-5095 — 50 s, under the client's 60 s timeout. Nothing is written
    // until every source has been read.
    const RlTagEvidence tags = rlReadTags(root, 50000);
    if (tags.failed)
        return pbErr(QStringLiteral("git_failed"),
            QStringLiteral("roadmap_log: place_by_release could not read the tags "
                           "— %1").arg(tags.error));

    // ---- The section tree, in document order --------------------------------
    QHash<qint64, int> idxOf;
    for (int i = 0; i < secs.size(); ++i) idxOf.insert(secs.at(i).sectionId, i);
    // A section's subtree: itself and the deeper sections after it in its file.
    const auto subtreeOf = [&](int idx) {
        QSet<qint64> out{secs.at(idx).sectionId};
        for (int j = idx + 1; j < secs.size(); ++j) {
            if (secs.at(j).sourcePath != secs.at(idx).sourcePath) continue;
            if (secs.at(j).level <= secs.at(idx).level) break;
            out.insert(secs.at(j).sectionId);
        }
        return out;
    };

    // Items in document order: by section order, then position.
    QVector<qint64> order;
    order.reserve(items.size());
    for (auto it = items.cbegin(); it != items.cend(); ++it) order << it.key();
    std::sort(order.begin(), order.end(), [&](qint64 a, qint64 b) {
        const auto &ia = items.value(a), &ib = items.value(b);
        const int sa = idxOf.value(ia.sectionId, int(secs.size()));
        const int sb = idxOf.value(ib.sectionId, int(secs.size()));
        if (sa != sb) return sa < sb;
        if (ia.position != ib.position) return ia.position < ib.position;
        return a < b;
    });
    QHash<qint64, int> ownItems;   // section -> items filed directly in it
    for (const qint64 pk : std::as_const(order)) ++ownItems[items.value(pk).sectionId];

    // § 2.4 — the marker, read back.
    struct Marker { bool placedBy = false; bool movedAfter = false; };
    QHash<qint64, Marker> markers;
    {
        QHash<qint64, int> lastPlaced, lastSection;
        int n = 0;
        for (const RoadmapStore::PlacementEvent &h : *historyOpt) {
            ++n;
            if (h.field == QLatin1String("placed_by")) lastPlaced[h.itemPk] = n;
            else lastSection[h.itemPk] = n;
        }
        for (auto it = lastPlaced.cbegin(); it != lastPlaced.cend(); ++it)
            markers[it.key()] = {true, lastSection.value(it.key(), 0) > it.value()};
    }

    // ---- § 2.2 — where each shipped item belongs ----------------------------
    struct Placement { QString version, source; };
    QHash<qint64, Placement> placement;
    struct Unplaced { qint64 pk; QString reason; };
    QVector<Unplaced> unplaced;
    int bothCount = 0, differCount = 0, multiCited = 0;
    QJsonArray disagreementSample;
    for (const qint64 pk : std::as_const(order)) {
        const RoadmapStore::ItemWrite &it = items.value(pk);
        if (it.status != QLatin1String("shipped")) continue;   // INV-5
        QStringList cited = cl.cited.value(it.id);
        std::sort(cited.begin(), cited.end(), rlVersionLess);
        const QString tagV = tags.versionOf.value(it.id);
        if (cited.size() > 1) ++multiCited;
        if (!cited.isEmpty()) {
            placement.insert(pk, {cited.first(), QStringLiteral("changelog")});
            if (!tagV.isEmpty()) {
                ++bothCount;
                if (tagV != cited.first()) {
                    ++differCount;
                    if (disagreementSample.size() < sampleLimit) {
                        QJsonObject d;
                        d[QStringLiteral("id")]        = it.id;
                        d[QStringLiteral("changelog")] = cited.first();
                        d[QStringLiteral("tag")]       = tagV;
                        disagreementSample.append(d);
                    }
                }
            }
        } else if (!tagV.isEmpty()) {
            placement.insert(pk, {tagV, QStringLiteral("tag")});
        } else {
            const bool unrel = cl.unreleased.contains(it.id) || tags.untagged.contains(it.id);
            unplaced.push_back({pk, unrel ? QStringLiteral("unreleased")
                                          : QStringLiteral("no_evidence")});
        }
    }

    // ---- § 2.3 — release sections ------------------------------------------
    QSet<QString> released = cl.versions;
    for (auto it = tags.tagDate.cbegin(); it != tags.tagDate.cend(); ++it)
        released.insert(it.key());
    QHash<int, QString> versionLed;   // section index -> numbered version
    for (int i = 0; i < secs.size(); ++i) {
        if (secs.at(i).level != 2) continue;
        const QString v = RoadmapIndex::versionOfTitle(secs.at(i).title);
        if (!v.isEmpty() && !v.endsWith(QLatin1String(".x"))) versionLed.insert(i, v);
    }
    QHash<int, QSet<qint64>> subtrees;
    for (auto it = versionLed.cbegin(); it != versionLed.cend(); ++it)
        subtrees.insert(it.key(), subtreeOf(it.key()));
    QList<int> ledOrder = versionLed.keys();
    std::sort(ledOrder.begin(), ledOrder.end());

    // Rule 1 — a version-led section holding an item this op placed there.
    QHash<QString, int> established;
    for (const int i : std::as_const(ledOrder)) {
        const QString v = versionLed.value(i);
        if (!released.contains(v) || established.contains(v)) continue;
        for (const qint64 pk : std::as_const(order))
            if (markers.value(pk).placedBy && subtrees[i].contains(items.value(pk).sectionId)) {
                established.insert(v, i);
                break;
            }
    }
    // § 2.4 — the items that stay because a human placed them, unless they
    // already sit in their own release section.
    QSet<qint64> humanCandidate;
    for (auto it = placement.cbegin(); it != placement.cend(); ++it) {
        const Marker m = markers.value(it.key());
        if (m.placedBy && m.movedAfter) { humanCandidate.insert(it.key()); continue; }
        if (m.placedBy) continue;
        const qint64 sid = items.value(it.key()).sectionId;
        for (auto e = established.cbegin(); e != established.cend(); ++e)
            if (e.key() != it->version && subtrees[e.value()].contains(sid))
                humanCandidate.insert(it.key());
    }
    QSet<QString> wanted;
    for (auto it = placement.cbegin(); it != placement.cend(); ++it)
        if (!humanCandidate.contains(it.key())) wanted.insert(it->version);
    QStringList wantedOrder(wanted.cbegin(), wanted.cend());
    std::sort(wantedOrder.begin(), wantedOrder.end(), rlVersionLess);

    QHash<QString, int> releaseIdx;     // version -> existing section index
    QHash<QString, QString> reuseRule;
    QStringList toCreate;
    for (const QString &v : std::as_const(wantedOrder)) {
        if (established.contains(v)) {
            releaseIdx.insert(v, established.value(v));
            reuseRule.insert(v, QStringLiteral("established"));
            continue;
        }
        // Rule 2 — every item left in its subtree after the moves belongs to v
        // or was placed by a human.
        int found = -1;
        for (const int i : std::as_const(ledOrder)) {
            if (versionLed.value(i) != v || rlIsRangeTitle(secs.at(i).title)) continue;
            bool ok = true;
            for (const qint64 pk : std::as_const(order)) {
                if (!subtrees[i].contains(items.value(pk).sectionId)) continue;
                if (humanCandidate.contains(pk)) continue;
                const auto p = placement.constFind(pk);
                if (p == placement.cend()) { ok = false; break; }   // stays, not v
                // A placed item for another release moves out.
            }
            if (ok) { found = i; break; }
        }
        if (found >= 0) {
            releaseIdx.insert(v, found);
            reuseRule.insert(v, QStringLiteral("reusable"));
        } else {
            toCreate << v;
        }
    }
    QSet<int> releaseSet;
    for (auto it = releaseIdx.cbegin(); it != releaseIdx.cend(); ++it) releaseSet.insert(it.value());

    // Every slug the project will hold, so a retitle or creation can refuse a
    // collision rather than invent a suffix (retitle_section's rule).
    QSet<QString> slugs;
    for (const auto &s : secs) slugs.insert(s.slug);

    // Retitles — every version-led section for a released version that is not
    // its release section.
    struct Retitle { int idx; QString title, slug; };
    QVector<Retitle> retitles;
    QHash<int, int> retitleOf;
    for (const int i : std::as_const(ledOrder)) {
        if (!released.contains(versionLed.value(i)) || releaseSet.contains(i)) continue;
        const RoadmapStore::SectionRow &s = secs.at(i);
        const QString title = rlWaveTitle(s.title);
        // An archive's slugs carry its `M-N-` prefix (rotate_minor); keep it.
        const QString oldBase = RoadmapIndex::slugifyHeading(s.title);
        const QString prefix = (s.slug != oldBase && s.slug.endsWith(oldBase))
            ? s.slug.left(s.slug.size() - oldBase.size()) : QString();
        const QString slug = prefix + RoadmapIndex::slugifyHeading(title);
        slugs.remove(s.slug);
        if (slug.isEmpty() || slugs.contains(slug))
            return pbErr(QStringLiteral("bad_args"),
                QStringLiteral("roadmap_log: retitling \"%1\" to \"%2\" gives the "
                               "slug \"%3\", which another section holds; nothing "
                               "was written").arg(s.title, title, slug));
        slugs.insert(slug);
        retitleOf.insert(i, int(retitles.size()));
        retitles.push_back({i, title, slug});
    }
    const auto titleNow = [&](int i) {
        return retitleOf.contains(i) ? retitles.at(retitleOf.value(i)).title : secs.at(i).title;
    };
    const auto slugNow = [&](int i) {
        return retitleOf.contains(i) ? retitles.at(retitleOf.value(i)).slug : secs.at(i).slug;
    };

    // Rule 3 — new sections, placed by ANTS-1878 § 2.3a among the live level-2
    // titles as they stand after the retitles, plus those already created.
    // `layout` is the final section order: an existing index, or -(k+1) for
    // the k-th created section.
    QVector<int> layout;
    for (int i = 0; i < secs.size(); ++i) layout << i;
    struct Created { QString version, title, slug, afterSlug, beforeSlug; };
    QVector<Created> created;
    const auto entryLive2 = [&](int e) {
        return e < 0 || (!secs.at(e).sourcePath && secs.at(e).level == 2);
    };
    const auto entryTitle = [&](int e) {
        return e < 0 ? created.at(-e - 1).title : titleNow(e);
    };
    const auto entrySlug = [&](int e) {
        return e < 0 ? created.at(-e - 1).slug : slugNow(e);
    };
    for (const QString &v : std::as_const(toCreate)) {
        QString date = tags.tagDate.value(v);
        if (date.isEmpty()) date = cl.headingDate.value(v);
        const QString title = date.isEmpty()
            ? QStringLiteral("%1 — released").arg(v)
            : QStringLiteral("%1 — released %2").arg(v, date);
        const QString slug = RoadmapIndex::slugifyHeading(title);
        if (slugs.contains(slug))
            return pbErr(QStringLiteral("bad_args"),
                QStringLiteral("roadmap_log: the new release section \"%1\" would "
                               "take the slug \"%2\", which another section holds; "
                               "nothing was written").arg(title, slug));
        slugs.insert(slug);

        const RlVerKey want = rlPlacementKey(v);
        QVector<int> h2;   // positions in `layout`
        for (int p = 0; p < layout.size(); ++p)
            if (entryLive2(layout.at(p))) h2 << p;
        int lowerAt = -1, higherAt = -1;
        RlVerKey lowerKey{}, higherKey{};
        int lastH2 = -1;
        for (int k = 0; k < h2.size(); ++k) {
            lastH2 = h2.at(k);
            const QString hv = RoadmapIndex::versionOfTitle(entryTitle(layout.at(h2.at(k))));
            if (hv.isEmpty()) continue;
            const RlVerKey key = rlPlacementKey(hv);
            if (key < want) {
                if (lowerAt < 0 || lowerKey < key) { lowerAt = h2.at(k); lowerKey = key; }
            } else if (higherAt < 0 || key < higherKey) {
                higherAt = k; higherKey = key;
            }
        }
        Created c{v, title, slug, QString(), QString()};
        int afterPos = -1, beforePos = -1;
        if (lowerAt >= 0)          afterPos  = lowerAt;
        else if (higherAt > 0)     afterPos  = h2.at(higherAt - 1);
        else if (higherAt == 0)    beforePos = h2.first();
        else if (lastH2 >= 0)      afterPos  = lastH2;   // no version-led section: the end
        int insertAt = int(layout.size());
        if (beforePos >= 0) {
            insertAt   = beforePos;
            c.beforeSlug = entrySlug(layout.at(beforePos));
        } else if (afterPos >= 0) {
            c.afterSlug = entrySlug(layout.at(afterPos));
            // Past the anchor's deeper sections (ANTS-4848), in its own file.
            insertAt = afterPos + 1;
            const int anchor = layout.at(afterPos);
            for (int p = afterPos + 1; p < layout.size(); ++p) {
                const int e = layout.at(p);
                if (e < 0) break;
                if (anchor >= 0 && secs.at(e).sourcePath != secs.at(anchor).sourcePath) continue;
                if (secs.at(e).level <= 2) break;
                insertAt = p + 1;
            }
        }
        created.push_back(c);
        layout.insert(insertAt, -int(created.size()));
    }
    QHash<QString, int> createdOf;
    for (int k = 0; k < created.size(); ++k) createdOf.insert(created.at(k).version, k);

    // ---- The moves -----------------------------------------------------------
    struct Move { qint64 pk; QString version, source; int fromIdx; int destIdx; int destCreated; };
    QVector<Move> moves;
    int alreadyPlaced = 0;
    for (const qint64 pk : std::as_const(order)) {
        const auto p = placement.constFind(pk);
        if (p == placement.cend()) continue;
        const qint64 sid = items.value(pk).sectionId;
        const int rIdx = releaseIdx.value(p->version, -1);
        if (rIdx >= 0 && subtrees[rIdx].contains(sid)) { ++alreadyPlaced; continue; }
        if (humanCandidate.contains(pk)) {
            unplaced.push_back({pk, QStringLiteral("human_placed")});
            continue;
        }
        moves.push_back({pk, p->version, p->source, idxOf.value(sid, -1), rIdx,
                         rIdx >= 0 ? -1 : createdOf.value(p->version, -1)});
    }
    QHash<qint64, int> leaving;
    for (const Move &m : std::as_const(moves)) ++leaving[items.value(m.pk).sectionId];

    // ---- One write -----------------------------------------------------------
    HistoryContext hist;
    hist.changedAt = rlHistoryStamp();
    bool overCap = false;
    const auto mutate = [&](QString *e) -> bool {
        for (const Retitle &r : std::as_const(retitles))
            if (!store.setSectionSlug(secs.at(r.idx).sectionId, r.slug, e)) return false;
        QHash<int, qint64> createdId;
        for (int k = 0; k < created.size(); ++k) {
            const auto id = store.addSection(projectId, created.at(k).slug,
                                             created.at(k).title, 2, 0, std::nullopt, e);
            if (!id) return false;
            createdId.insert(k, *id);
        }
        // Renumber in `layout` order, writing only rows that change.
        for (int p = 0; p < layout.size(); ++p) {
            const int en = layout.at(p);
            if (en < 0) {
                if (!store.updateSection(createdId.value(-en - 1), created.at(-en - 1).title,
                                         2, p, std::nullopt, e))
                    return false;
                continue;
            }
            const RoadmapStore::SectionRow &s = secs.at(en);
            if (s.position == p && !retitleOf.contains(en)) continue;
            if (!store.updateSection(s.sectionId, titleNow(en), s.level, p, s.parentId, e))
                return false;
        }
        QHash<qint64, int> nextPos;
        for (const Move &m : std::as_const(moves)) {
            const qint64 dest = m.destIdx >= 0 ? secs.at(m.destIdx).sectionId
                                               : createdId.value(m.destCreated);
            if (!nextPos.contains(dest)) {
                int maxPos = -1;
                if (m.destIdx >= 0) {
                    const auto els = store.listElements(dest, e);
                    if (!els) return false;
                    for (const RoadmapStore::ElementRow &el : *els)
                        maxPos = std::max(maxPos, el.position);
                }
                nextPos.insert(dest, maxPos);
            }
            const RoadmapStore::ItemWrite &it = items.value(m.pk);
            if (it.sectionId != 0 && !store.unfileItem(m.pk, e)) return false;
            if (!store.fileItem(m.pk, dest, ++nextPos[dest], e)) return false;
            // § 2.4 — the `section` row amend_field writes, then the marker.
            hist.record(m.pk, QStringLiteral("section"),
                        m.fromIdx >= 0 ? secs.at(m.fromIdx).slug : QString(),
                        m.destIdx >= 0 ? slugNow(m.destIdx) : created.at(m.destCreated).slug);
            hist.record(m.pk, QStringLiteral("placed_by"), QString(),
                        QStringLiteral("place_by_release"));
            if (!rlStampModified(store, m.pk, e)) return false;
        }
        // § 2.4 — the marker is what every later run reads, and over the cap
        // rlFlushHistory() would drop it while reporting success.
        if (!hist.pending.isEmpty() && store.historyWouldExceedCap(hist.pendingBytes())) {
            overCap = true;
            *e = QStringLiteral("place_by_release: its history rows would exceed the "
                                "store's history cap, and without them a later run "
                                "cannot tell its own placements from a human's");
            return false;
        }
        return rlFlushHistory(store, hist, e);
    };

    QJsonObject env;
    const bool anything = !moves.isEmpty() || !created.isEmpty() || !retitles.isEmpty();
    RoadmapRender::Outcome outcome;
    if (anything) {
        QString writeErr;
        const auto r = RoadmapWrite::commitAndRender(
            store, projectId, root, roadmapPath, dryRun, mutate, &outcome, &writeErr);
        if (rcRoadmapWriteRefused(env, r, writeErr, outcome)) {
            if (overCap) env[QStringLiteral("code")] = QStringLiteral("store_failed");
            return QJsonDocument(env);
        }
    }

    // ---- § 2.6 — the reply ---------------------------------------------------
    env[QStringLiteral("ok")] = true;
    env[QStringLiteral("op")] = QStringLiteral("place_by_release");
    if (dryRun) env[QStringLiteral("dry_run")] = true;
    const auto capped = [&](QJsonObject &out, const QString &key, const QJsonArray &rows,
                            int total) {
        out[key] = rows;
        if (total > rows.size()) out[key + QStringLiteral("_truncated")] = true;
    };

    QJsonArray moved;
    for (const Move &m : std::as_const(moves)) {
        if (moved.size() >= sampleLimit) break;
        QJsonObject o;
        o[QStringLiteral("id")]      = items.value(m.pk).id;
        o[QStringLiteral("from")]    = m.fromIdx >= 0 ? secs.at(m.fromIdx).slug : QString();
        o[QStringLiteral("to")]      = m.destIdx >= 0 ? slugNow(m.destIdx)
                                                      : created.at(m.destCreated).slug;
        o[QStringLiteral("version")] = m.version;
        o[QStringLiteral("source")]  = m.source;
        moved.append(o);
    }
    env[QStringLiteral("moved_count")] = int(moves.size());
    capped(env, QStringLiteral("moved"), moved, int(moves.size()));
    env[QStringLiteral("already_placed_count")] = alreadyPlaced;

    QJsonArray unplacedRows;
    QJsonObject byReason{{QStringLiteral("unreleased"), 0}, {QStringLiteral("no_evidence"), 0},
                         {QStringLiteral("human_placed"), 0}};
    for (const Unplaced &u : std::as_const(unplaced)) {
        byReason[u.reason] = byReason.value(u.reason).toInt() + 1;
        if (unplacedRows.size() >= sampleLimit) continue;
        const int si = idxOf.value(items.value(u.pk).sectionId, -1);
        QJsonObject o;
        o[QStringLiteral("id")]      = items.value(u.pk).id;
        o[QStringLiteral("reason")]  = u.reason;
        o[QStringLiteral("section")] = si >= 0 ? slugNow(si) : QString();
        unplacedRows.append(o);
    }
    env[QStringLiteral("unplaced_count")] = int(unplaced.size());
    capped(env, QStringLiteral("unplaced"), unplacedRows, int(unplaced.size()));
    env[QStringLiteral("unplaced_by_reason")] = byReason;

    QJsonArray createdRows;
    for (const Created &c : std::as_const(created)) {
        QJsonObject o;
        o[QStringLiteral("slug")]          = c.slug;
        o[QStringLiteral("title")]         = c.title;
        o[QStringLiteral("after_section")] = c.afterSlug;
        if (!c.beforeSlug.isEmpty()) o[QStringLiteral("before_section")] = c.beforeSlug;
        createdRows.append(o);
    }
    env[QStringLiteral("sections_created")] = createdRows;
    QJsonArray reusedRows;
    for (const QString &v : std::as_const(wantedOrder)) {
        if (!releaseIdx.contains(v)) continue;
        QJsonObject o;
        o[QStringLiteral("slug")]    = slugNow(releaseIdx.value(v));
        o[QStringLiteral("version")] = v;
        o[QStringLiteral("rule")]    = reuseRule.value(v);
        reusedRows.append(o);
    }
    env[QStringLiteral("sections_reused")] = reusedRows;
    QJsonArray retitledRows;
    for (const Retitle &r : std::as_const(retitles)) {
        QJsonObject o;
        o[QStringLiteral("from_slug")] = secs.at(r.idx).slug;
        o[QStringLiteral("to_slug")]   = r.slug;
        o[QStringLiteral("from")]      = secs.at(r.idx).title;
        o[QStringLiteral("to")]        = r.title;
        retitledRows.append(o);
    }
    env[QStringLiteral("sections_retitled")] = retitledRows;
    QJsonArray emptied;
    for (int i = 0; i < secs.size(); ++i) {
        const int own = ownItems.value(secs.at(i).sectionId);
        if (own > 0 && leaving.value(secs.at(i).sectionId) == own) emptied.append(slugNow(i));
    }
    env[QStringLiteral("sections_emptied")] = emptied;

    QJsonObject dis;
    dis[QStringLiteral("both_count")]   = bothCount;
    dis[QStringLiteral("differ_count")] = differCount;
    dis[QStringLiteral("sample")]       = disagreementSample;
    if (differCount > disagreementSample.size()) dis[QStringLiteral("sample_truncated")] = true;
    env[QStringLiteral("disagreements")]     = dis;
    env[QStringLiteral("multi_cited_count")] = multiCited;
    QJsonObject sources;
    sources[QStringLiteral("changelog")] =
        cl.relPath.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(cl.relPath);
    sources[QStringLiteral("stable_tags")] = tags.stableTags;
    env[QStringLiteral("sources")] = sources;
    if (anything) {
        rlAttachHistoryNote(env, store, hist);
        rcRoadmapWriteFields(env, outcome, dryRun);   // ANTS-4463
    }
    return QJsonDocument(env);
}
