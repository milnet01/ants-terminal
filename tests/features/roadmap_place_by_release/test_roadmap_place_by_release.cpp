// Feature-conformance test for ANTS-5315 step 4 — roadmap_log
// op:"place_by_release". Contract:
// docs/specs/ANTS-5315-release-sections-migration.md § 3, INV-1 to INV-12.
//
// The fixture builds a REAL git repository with stable and pre-release tags,
// a CHANGELOG and a migrated store, as tests/features/roadmap_backfill_dates
// does: the op reads tags and commit subjects, so a fake would assert against
// the harness. Every case goes through RemoteControl::cmdRoadmapLog, because
// the dry-run-first rule and plan_id live in that wrapper.
//
// The red-run trap: on pre-change source the op does not exist, so every
// request refuses bad_op_combo. Each case asserts a value the op must produce,
// never merely that nothing moved.
//
// What the fixture holds, and where § 2.2 / § 2.3 send it:
//
//   DEMO-0001 ✅  cited [0.2.0] and [0.3.0], subject commit under v0.1.0
//                 → 0.2.0 by CHANGELOG; a disagreement and multi-cited
//   DEMO-0002 ✅  uncited, subject commit under v0.2.0 → 0.2.0 by tag
//   DEMO-0004 ✅  subject commit under v0.4.0-rc1 only, on its own branch
//                 → unreleased
//   DEMO-0005 📋  cited [0.2.0] → never moves (INV-5)
//   DEMO-0006 📋  in "0.1.0–0.1.9 — wave" → that section is retitled
//   DEMO-0007 📋  in "0.10.0 — ten wave" → retitled; 0.10.0 gets a new section
//   DEMO-0008 📋  in "1.0.0 — target" (1.0.0 is not released)
//   DEMO-0009 ✅  subject commit under v0.9.0 → new 0.9.0 section
//   DEMO-0010 ✅  subject commit under v0.10.0 → new 0.10.0 section
//   DEMO-0011 ✅  subject "DEMO-0011 fix …" under v0.3.0 → new 0.3.0 section
//   DEMO-0012 ✅  cited only under [Unreleased] → unreleased
//   DEMO-0013 ✅  cited [0.1.0] → the archived, empty "0.1.0 — first" section
//   DEMO-0014 ✅  no evidence, a shipped date → no_evidence, stays

#include "../../_support/expect.h"
#include "../../_support/xdg_guard.h"

#include "remotecontrol.h"
#include "roadmapindex.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"
#include "../../_support/roadmapstoreaccess.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>

#include <memory>

ANTS_TEST_SCOPE();

namespace {

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QByteArray readFile(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// NEVER default-construct RoadmapStore: defaultPath() resolves the developer's
// REAL store. Safe here only because XdgGuard has redirected XDG_DATA_HOME.
std::unique_ptr<RoadmapStore> openStore(RoadmapStore::Access access) {
    auto store = std::make_unique<RoadmapStore>(RoadmapStore::defaultPath(),
                                                RoadmapStore::kDefaultHistoryCapBytes,
                                                access);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return nullptr;
    }
    return store;
}

bool git(const QString &dir, const QStringList &args, const QString &date = {}) {
    QProcess p;
    p.setWorkingDirectory(dir);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!date.isEmpty()) {
        // A lightweight tag's creator date is its commit's committer date.
        env.insert(QStringLiteral("GIT_AUTHOR_DATE"), date);
        env.insert(QStringLiteral("GIT_COMMITTER_DATE"), date);
    }
    p.setProcessEnvironment(env);
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted(3000) || !p.waitForFinished(15000)) {
        ADD_FAILURE() << "git " << args.join(QLatin1Char(' ')).toStdString()
                      << " did not run";
        return false;
    }
    if (p.exitCode() != 0) {
        ADD_FAILURE() << "git " << args.join(QLatin1Char(' ')).toStdString()
                      << " exited " << p.exitCode() << ": "
                      << QString::fromUtf8(p.readAllStandardError()).toStdString();
        return false;
    }
    return true;
}

// Past kRoadmapMinParseableSize (1024 B).
const char *kPad =
    "Intro paragraph that exists purely to pad this fixture past the 1 KiB\n"
    "minimum-parseable-size gate the roadmap_log write paths enforce before\n"
    "they will trust an ants-v1 walk. Lorem ipsum dolor sit amet, consectetur\n"
    "adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore\n"
    "magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco\n"
    "laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in\n"
    "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla\n"
    "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa\n"
    "qui officia deserunt mollit anim id est laborum. Sed ut perspiciatis unde\n"
    "omnis iste natus error sit voluptatem accusantium doloremque laudantium,\n"
    "totam rem aperiam eaque ipsa quae ab illo inventore veritatis et quasi\n"
    "architecto beatae vitae dicta sunt explicabo nemo enim ipsam voluptatem.\n";

constexpr const char *kShipped = "\xE2\x9C\x85";
constexpr const char *kPlanned = "\xF0\x9F\x93\x8B";

QByteArray bullet(const char *emoji, const char *id, const char *headline) {
    return QByteArray("- ") + emoji + " [" + id + "] **" + headline + "**\n"
           "  Layman: A thing.\n"
           "  Kind: fix.\n"
           "  Source: seed.\n\n";
}

const QString kWaveRange = QStringLiteral("0.1.0–0.1.9 — wave");
const QString kWaveTen   = QStringLiteral("0.10.0 — ten wave");
const QString kTarget    = QStringLiteral("1.0.0 — target");
const QString kFirst     = QStringLiteral("0.1.0 — first");

// `noEvidenceUnderFirst` puts DEMO-0014 in the 0.1.0 section and leaves that
// section live, for INV-4's "sitting under ## 0.1.0" case.
QByteArray fixtureRoadmap(bool noEvidenceUnderFirst) {
    QByteArray b = "# Demo \xE2\x80\x94 Roadmap\n\n";
    b += kPad;
    b += "\n## Work\n\n";
    b += bullet(kShipped, "DEMO-0001", "Cited twice, tagged early.");
    b += bullet(kShipped, "DEMO-0002", "Tagged only.");
    b += bullet(kShipped, "DEMO-0004", "Only in a release candidate.");
    b += bullet(kPlanned, "DEMO-0005", "Open, but cited.");
    b += bullet(kShipped, "DEMO-0009", "Tagged in nine.");
    b += bullet(kShipped, "DEMO-0010", "Tagged in ten.");
    b += bullet(kShipped, "DEMO-0011", "Tagged in three.");
    b += bullet(kShipped, "DEMO-0012", "Unreleased in the changelog.");
    b += bullet(kShipped, "DEMO-0013", "Cited in the first release.");
    if (!noEvidenceUnderFirst)
        b += bullet(kShipped, "DEMO-0014", "No evidence anywhere.");
    b += "## " + kWaveRange.toUtf8() + "\n\n";
    b += bullet(kPlanned, "DEMO-0006", "Open in the range wave.");
    b += "## " + kWaveTen.toUtf8() + "\n\n";
    b += bullet(kPlanned, "DEMO-0007", "Open in the ten wave.");
    b += "## " + kTarget.toUtf8() + "\n\n";
    b += bullet(kPlanned, "DEMO-0008", "Open in the target.");
    b += "## " + kFirst.toUtf8() + "\n\n";
    if (noEvidenceUnderFirst)
        b += bullet(kShipped, "DEMO-0014", "No evidence anywhere.");
    else
        b += "Nothing filed here yet.\n\n";
    return b;
}

const char *kChangelog =
    "# Changelog\n\n"
    "## [Unreleased]\n\n### Added\n\n- DEMO-0012 lands next.\n\n"
    "## [0.3.0] - 2026-03-01\n\n### Changed\n\n- DEMO-0001 follow-up.\n\n"
    "## [0.2.0] - 2026-02-01\n\n### Added\n\n- DEMO-0001 the feature.\n"
    "- DEMO-0005 an open item named early.\n\n"
    "## [0.1.0] - 2026-01-01\n\n### Added\n\n- DEMO-0013 the first thing.\n";

struct Fixture {
    QString root;
    qint64  projectId = -1;
};

bool commitAndTag(const QString &root, const QString &subject, const QString &tag,
                  const QString &date) {
    if (!git(root, {QStringLiteral("commit"), QStringLiteral("-q"),
                    QStringLiteral("--allow-empty"), QStringLiteral("-m"), subject},
             date))
        return false;
    return tag.isEmpty() || git(root, {QStringLiteral("tag"), tag});
}

Fixture seed(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
             bool noEvidenceUnderFirst = false) {
    Fixture fx;
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    QDir().mkpath(rawRoot);
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"),
                   fixtureRoadmap(noEvidenceUnderFirst)) ||
        !writeFile(rawRoot + QStringLiteral("/CHANGELOG.md"), kChangelog))
        return fx;
    const QString root = QFileInfo(rawRoot).canonicalFilePath();

    if (!git(root, {QStringLiteral("init"), QStringLiteral("-q")}) ||
        !git(root, {QStringLiteral("config"), QStringLiteral("user.email"),
                    QStringLiteral("t@example.invalid")}) ||
        !git(root, {QStringLiteral("config"), QStringLiteral("user.name"),
                    QStringLiteral("T")}))
        return fx;
    // One commit per tag, in tag order, on one line of history.
    if (!commitAndTag(root, QStringLiteral("DEMO-0001: the early cut"),
                      QStringLiteral("v0.1.0"), QStringLiteral("2026-01-01T10:00:00 +0000")) ||
        !commitAndTag(root, QStringLiteral("DEMO-0002: tagged only"),
                      QStringLiteral("v0.2.0"), QStringLiteral("2026-02-01T10:00:00 +0000")) ||
        !commitAndTag(root, QStringLiteral("DEMO-0011 fix without a colon"),
                      QStringLiteral("v0.3.0"), QStringLiteral("2026-03-01T10:00:00 +0000")) ||
        // INV-11 — the candidate sits on a branch no stable tag contains.
        !git(root, {QStringLiteral("checkout"), QStringLiteral("-q"),
                    QStringLiteral("-b"), QStringLiteral("rc")}) ||
        !commitAndTag(root, QStringLiteral("DEMO-0004: candidate only"),
                      QStringLiteral("v0.4.0-rc1"), QStringLiteral("2026-04-01T10:00:00 +0000")) ||
        !git(root, {QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("-")}) ||
        !commitAndTag(root, QStringLiteral("Unrelated: mentions DEMO-0014 in passing"),
                      QString(), QStringLiteral("2026-04-15T10:00:00 +0000")) ||
        !commitAndTag(root, QStringLiteral("DEMO-0009: nine"),
                      QStringLiteral("v0.9.0"), QStringLiteral("2026-09-01T10:00:00 +0000")) ||
        !commitAndTag(root, QStringLiteral("DEMO-0010: ten"),
                      QStringLiteral("v0.10.0"), QStringLiteral("2026-10-01T10:00:00 +0000")))
        return fx;

    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return fx;
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) {
        ADD_FAILURE() << "findRoadmaps: " << err.toStdString();
        return fx;
    }
    const auto plan =
        RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"), QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-14T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) {
        ADD_FAILURE() << "migration load: " << out.error.toStdString();
        return fx;
    }

    // INV-4 — a shipped date must not become a placement.
    if (const auto pk = store->findItem(out.projectId, QStringLiteral("DEMO-0014"), &err))
        store->setItemField(*pk, QStringLiteral("shipped"), QStringLiteral("2026-02-15"), &err);

    // INV-8 — the 0.1.0 section lives in an archive file.
    if (!noEvidenceUnderFirst) {
        const auto sid = store->findSection(
            out.projectId, RoadmapIndex::slugifyHeading(kFirst), &err);
        if (!sid || !store->setSectionSource(*sid, QStringLiteral("docs/roadmap/0.1.md"), &err)) {
            ADD_FAILURE() << "archiving 0.1.0: " << err.toStdString();
            return fx;
        }
    }
    fx.projectId = out.projectId;
    fx.root      = root;
    return fx;
}

QJsonObject roadmapLog(const QJsonObject &req) {
    RemoteControl rc(nullptr);
    return rc.cmdRoadmapLog(req).object();
}

QJsonObject dryRun(const Fixture &fx, int sampleLimit = 200) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")]   = fx.root;
    req[QStringLiteral("op")]           = QStringLiteral("place_by_release");
    req[QStringLiteral("dry_run")]      = true;
    req[QStringLiteral("sample_limit")] = sampleLimit;
    return roadmapLog(req);
}

// Each RemoteControl is a fresh object, but the plan stash is process-wide.
QJsonObject apply(const Fixture &fx, const QString &planId) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = fx.root;
    req[QStringLiteral("plan_id")]    = planId;
    return roadmapLog(req);
}

std::string dump(const QJsonObject &o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

#define ASSERT_VERB_OK(resp) \
    ASSERT_TRUE((resp).value(QStringLiteral("ok")).toBool()) << dump(resp)

// Dry run, then apply its plan. Returns the apply's reply.
QJsonObject dryRunThenApply(const Fixture &fx, QJsonObject *dryOut = nullptr) {
    const QJsonObject dry = dryRun(fx);
    if (!dry.value(QStringLiteral("ok")).toBool()) {
        ADD_FAILURE() << "dry run: " << dump(dry);
        return dry;
    }
    if (dryOut) *dryOut = dry;
    return apply(fx, dry.value(QStringLiteral("plan_id")).toString());
}

QJsonObject rowFor(const QJsonArray &rows, const QString &key, const QString &value) {
    for (const QJsonValue &v : rows)
        if (v.toObject().value(key).toString() == value) return v.toObject();
    return {};
}

int historyCount(qint64 projectId, const QString &field = QString(),
                 qint64 afterHistoryId = 0) {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return -1;
    QSqlQuery q(RoadmapStoreTestAccess::db(*store));
    q.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM history h JOIN item i ON i.item_pk = h.item_pk "
        "WHERE i.project_id = ? AND h.history_id > ? AND (? = '' OR h.field = ?)"));
    q.addBindValue(projectId);
    q.addBindValue(afterHistoryId);
    q.addBindValue(field.isNull() ? QStringLiteral("") : field);
    q.addBindValue(field.isNull() ? QStringLiteral("") : field);
    if (!q.exec() || !q.next()) {
        ADD_FAILURE() << "history count: " << q.lastError().text().toStdString();
        return -1;
    }
    return q.value(0).toInt();
}

qint64 maxHistoryId() {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return -1;
    QSqlQuery q(RoadmapStoreTestAccess::db(*store));
    if (!q.exec(QStringLiteral("SELECT COALESCE(MAX(history_id), 0) FROM history")) || !q.next())
        return -1;
    return q.value(0).toLongLong();
}

QString sectionSlugOf(qint64 projectId, const QString &id) {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return {};
    QString err;
    const auto pk = store->findItem(projectId, id, &err);
    if (!pk) return {};
    const auto item = store->readItem(*pk, &err);
    if (!item) return {};
    const auto sec = store->readSection(item->sectionId, &err);
    return sec ? sec->slug : QString();
}

}  // namespace

// ------------------------------------------------------------------ INV-1 ---
TEST(RoadmapPlaceByRelease, Inv1ADirectCallRefusesAndWritesNothing) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Fixture fx = seed(guard, tmp);
    ASSERT_FALSE(fx.root.isEmpty());

    const QByteArray before = readFile(fx.root + QStringLiteral("/ROADMAP.md"));
    const int historyBefore = historyCount(fx.projectId);

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = fx.root;
    req[QStringLiteral("op")]         = QStringLiteral("place_by_release");
    const QJsonObject resp = roadmapLog(req);
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(), QStringLiteral("bad_args"))
        << dump(resp);
    EXPECT_TRUE(resp.value(QStringLiteral("hint")).toString().contains(
        QStringLiteral("dry_run:true"))) << dump(resp);

    // The internal apply mark is not a field a caller can send.
    req[QStringLiteral("_plan_apply")] = true;
    EXPECT_EQ(roadmapLog(req).value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"));

    EXPECT_EQ(historyCount(fx.projectId), historyBefore);
    EXPECT_EQ(readFile(fx.root + QStringLiteral("/ROADMAP.md")), before);
}

// ------------------------------------------------------------------ INV-2 ---
TEST(RoadmapPlaceByRelease, Inv2TheApplyWritesWhatTheDryRunReported) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Fixture fx = seed(guard, tmp);
    ASSERT_FALSE(fx.root.isEmpty());

    const QByteArray before = readFile(fx.root + QStringLiteral("/ROADMAP.md"));
    const int historyBefore = historyCount(fx.projectId);
    const QJsonObject dry = dryRun(fx);
    ASSERT_VERB_OK(dry);
    EXPECT_TRUE(dry.value(QStringLiteral("dry_run")).toBool());
    EXPECT_GT(dry.value(QStringLiteral("moved_count")).toInt(), 0) << dump(dry);
    EXPECT_EQ(historyCount(fx.projectId), historyBefore);
    EXPECT_EQ(readFile(fx.root + QStringLiteral("/ROADMAP.md")), before);

    const QJsonObject applied = apply(fx, dry.value(QStringLiteral("plan_id")).toString());
    ASSERT_VERB_OK(applied);
    EXPECT_FALSE(applied.value(QStringLiteral("dry_run")).toBool());
    for (const char *key : {"moved", "sections_created", "sections_retitled"}) {
        SCOPED_TRACE(key);
        EXPECT_EQ(applied.value(QLatin1String(key)).toArray(),
                  dry.value(QLatin1String(key)).toArray());
    }
    EXPECT_NE(readFile(fx.root + QStringLiteral("/ROADMAP.md")), before);
}

// ------------------------------------------------------------------ INV-3 ---
TEST(RoadmapPlaceByRelease, Inv3LowestChangelogCitationWinsThenTags) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Fixture fx = seed(guard, tmp);
    ASSERT_FALSE(fx.root.isEmpty());

    const QJsonObject dry = dryRun(fx);
    ASSERT_VERB_OK(dry);
    const QJsonArray moved = dry.value(QStringLiteral("moved")).toArray();
    const QJsonObject a = rowFor(moved, QStringLiteral("id"), QStringLiteral("DEMO-0001"));
    EXPECT_EQ(a.value(QStringLiteral("version")).toString(), QStringLiteral("0.2.0")) << dump(dry);
    EXPECT_EQ(a.value(QStringLiteral("source")).toString(), QStringLiteral("changelog"));
    const QJsonObject b = rowFor(moved, QStringLiteral("id"), QStringLiteral("DEMO-0002"));
    EXPECT_EQ(b.value(QStringLiteral("version")).toString(), QStringLiteral("0.2.0"));
    EXPECT_EQ(b.value(QStringLiteral("source")).toString(), QStringLiteral("tag"));
    EXPECT_EQ(a.value(QStringLiteral("to")).toString(), b.value(QStringLiteral("to")).toString());
    // A subject with no colon still names its item.
    EXPECT_EQ(rowFor(moved, QStringLiteral("id"), QStringLiteral("DEMO-0011"))
                  .value(QStringLiteral("version")).toString(), QStringLiteral("0.3.0"));

    const QJsonObject dis = dry.value(QStringLiteral("disagreements")).toObject();
    EXPECT_EQ(dis.value(QStringLiteral("differ_count")).toInt(), 1) << dump(dis);
    const QJsonObject row = rowFor(dis.value(QStringLiteral("sample")).toArray(),
                                   QStringLiteral("id"), QStringLiteral("DEMO-0001"));
    EXPECT_EQ(row.value(QStringLiteral("changelog")).toString(), QStringLiteral("0.2.0"));
    EXPECT_EQ(row.value(QStringLiteral("tag")).toString(), QStringLiteral("0.1.0"));
    EXPECT_EQ(dry.value(QStringLiteral("multi_cited_count")).toInt(), 1);
    EXPECT_EQ(dry.value(QStringLiteral("sources")).toObject()
                  .value(QStringLiteral("stable_tags")).toInt(), 5);
}

// § 2.2 item 1 — every `## [X.Y.Z]` section citing the id is a candidate,
// whatever `###` heading the citation sits under. Measured on Ants Terminal's
// CHANGELOG 2026-10-19: reading only entries under the six Keep-a-Changelog
// categories lost 110 placements.
TEST(RoadmapPlaceByRelease, Inv3ACitationUnderAnyHeadingCounts) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Fixture fx = seed(guard, tmp);
    ASSERT_FALSE(fx.root.isEmpty());
    QByteArray changelog(kChangelog);
    changelog.replace("## [0.1.0]",
                      "### \xF0\x9F\x94\x8C MCP fold-in \xE2\x80\x94 pull 5\n\n"
                      "- DEMO-0014 named under a topic heading.\n\n## [0.1.0]");
    ASSERT_TRUE(writeFile(fx.root + QStringLiteral("/CHANGELOG.md"), changelog));

    const QJsonObject dry = dryRun(fx);
    ASSERT_VERB_OK(dry);
    const QJsonObject row = rowFor(dry.value(QStringLiteral("moved")).toArray(),
                                   QStringLiteral("id"), QStringLiteral("DEMO-0014"));
    EXPECT_EQ(row.value(QStringLiteral("version")).toString(), QStringLiteral("0.2.0"))
        << dump(dry);
    EXPECT_EQ(row.value(QStringLiteral("source")).toString(), QStringLiteral("changelog"));
}

// ------------------------------------------------------------------ INV-4 ---
TEST(RoadmapPlaceByRelease, Inv4TitlesAndDatesNeverPlace) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Fixture fx = seed(guard, tmp, /*noEvidenceUnderFirst=*/true);
    ASSERT_FALSE(fx.root.isEmpty());

    const QString home = sectionSlugOf(fx.projectId, QStringLiteral("DEMO-0014"));
    ASSERT_EQ(home, RoadmapIndex::slugifyHeading(kFirst));
    const QJsonObject applied = dryRunThenApply(fx);
    ASSERT_VERB_OK(applied);
    const QJsonObject row = rowFor(applied.value(QStringLiteral("unplaced")).toArray(),
                                   QStringLiteral("id"), QStringLiteral("DEMO-0014"));
    EXPECT_EQ(row.value(QStringLiteral("reason")).toString(), QStringLiteral("no_evidence"))
        << dump(applied);
    EXPECT_TRUE(rowFor(applied.value(QStringLiteral("moved")).toArray(),
                       QStringLiteral("id"), QStringLiteral("DEMO-0014")).isEmpty());
    // It stays in its section, which is retitled because it cannot be reused
    // as 0.1.0's release section, so its slug follows the new title.
    EXPECT_EQ(sectionSlugOf(fx.projectId, QStringLiteral("DEMO-0014")),
              RoadmapIndex::slugifyHeading(QStringLiteral("first (0.1.0 wave)")));
}

// ------------------------------------------------------------------ INV-5 ---
TEST(RoadmapPlaceByRelease, Inv5OnlyShippedItemsMove) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Fixture fx = seed(guard, tmp);
    ASSERT_FALSE(fx.root.isEmpty());

    const auto elementOf = [&](const QString &id) {
        auto store = openStore(RoadmapStore::Access::Interactive);
        QSqlQuery q(RoadmapStoreTestAccess::db(*store));
        q.prepare(QStringLiteral(
            "SELECT e.section_id, e.position FROM element e JOIN item i "
            "ON i.item_pk = e.item_pk WHERE i.project_id = ? AND i.id = ?"));
        q.addBindValue(fx.projectId);
        q.addBindValue(id);
        return (q.exec() && q.next())
            ? QStringLiteral("%1/%2").arg(q.value(0).toLongLong()).arg(q.value(1).toInt())
            : QString();
    };
    const QString openBefore = elementOf(QStringLiteral("DEMO-0005"));
    const QString shippedBefore = elementOf(QStringLiteral("DEMO-0001"));
    ASSERT_FALSE(openBefore.isEmpty());

    const QJsonObject applied = dryRunThenApply(fx);
    ASSERT_VERB_OK(applied);
    EXPECT_FALSE(rowFor(applied.value(QStringLiteral("moved")).toArray(),
                        QStringLiteral("id"), QStringLiteral("DEMO-0001")).isEmpty());
    EXPECT_NE(elementOf(QStringLiteral("DEMO-0001")), shippedBefore);
    EXPECT_EQ(elementOf(QStringLiteral("DEMO-0005")), openBefore);
}

// ------------------------------------------------------------------ INV-6 ---
TEST(RoadmapPlaceByRelease, Inv6AHumanMoveIsLeftAlone) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Fixture fx = seed(guard, tmp);
    ASSERT_FALSE(fx.root.isEmpty());

    ASSERT_VERB_OK(dryRunThenApply(fx));
    QJsonObject move;
    move[QStringLiteral("caller_cwd")] = fx.root;
    move[QStringLiteral("op")]         = QStringLiteral("amend_field");
    move[QStringLiteral("id")]         = QStringLiteral("DEMO-0002");
    move[QStringLiteral("field")]      = QStringLiteral("section");
    move[QStringLiteral("value")]      = QStringLiteral("work");
    ASSERT_VERB_OK(roadmapLog(move));

    const QJsonObject again = dryRunThenApply(fx);
    ASSERT_VERB_OK(again);
    const QJsonObject row = rowFor(again.value(QStringLiteral("unplaced")).toArray(),
                                   QStringLiteral("id"), QStringLiteral("DEMO-0002"));
    EXPECT_EQ(row.value(QStringLiteral("reason")).toString(), QStringLiteral("human_placed"))
        << dump(again);
    EXPECT_EQ(sectionSlugOf(fx.projectId, QStringLiteral("DEMO-0002")), QStringLiteral("work"));
}

// ------------------------------------------------------------------ INV-7 ---
TEST(RoadmapPlaceByRelease, Inv7ASecondRunPlansNothing) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Fixture fx = seed(guard, tmp);
    ASSERT_FALSE(fx.root.isEmpty());

    const QJsonObject first = dryRunThenApply(fx);
    ASSERT_VERB_OK(first);
    ASSERT_GT(first.value(QStringLiteral("moved_count")).toInt(), 0);
    const QJsonObject again = dryRun(fx);
    ASSERT_VERB_OK(again);
    EXPECT_EQ(again.value(QStringLiteral("moved_count")).toInt(), 0) << dump(again);
    EXPECT_TRUE(again.value(QStringLiteral("sections_created")).toArray().isEmpty());
    EXPECT_TRUE(again.value(QStringLiteral("sections_retitled")).toArray().isEmpty());
    EXPECT_EQ(again.value(QStringLiteral("already_placed_count")).toInt(),
              first.value(QStringLiteral("moved_count")).toInt());
}

// ------------------------------------------------------------------ INV-8 ---
TEST(RoadmapPlaceByRelease, Inv8AnArchivedReleaseSectionIsReused) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Fixture fx = seed(guard, tmp);
    ASSERT_FALSE(fx.root.isEmpty());

    const QJsonObject applied = dryRunThenApply(fx);
    ASSERT_VERB_OK(applied);
    const QString firstSlug = RoadmapIndex::slugifyHeading(kFirst);
    EXPECT_EQ(sectionSlugOf(fx.projectId, QStringLiteral("DEMO-0013")), firstSlug)
        << dump(applied);
    const QJsonObject reused = rowFor(applied.value(QStringLiteral("sections_reused")).toArray(),
                                      QStringLiteral("slug"), firstSlug);
    EXPECT_EQ(reused.value(QStringLiteral("rule")).toString(), QStringLiteral("reusable"));
    for (const QJsonValue &v : applied.value(QStringLiteral("sections_created")).toArray())
        EXPECT_NE(RoadmapIndex::versionOfTitle(v.toObject().value(QStringLiteral("title")).toString()),
                  QStringLiteral("0.1.0")) << dump(applied);
}

// ------------------------------------------------------------------ INV-9 ---
TEST(RoadmapPlaceByRelease, Inv9OnlyReleaseSectionsLeadWithAReleasedVersion) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Fixture fx = seed(guard, tmp);
    ASSERT_FALSE(fx.root.isEmpty());

    const QJsonObject applied = dryRunThenApply(fx);
    ASSERT_VERB_OK(applied);
    const QJsonObject range = rowFor(applied.value(QStringLiteral("sections_retitled")).toArray(),
                                     QStringLiteral("from"), kWaveRange);
    EXPECT_EQ(range.value(QStringLiteral("to")).toString(),
              QStringLiteral("wave (0.1.0–0.1.9 wave)")) << dump(applied);
    const QJsonObject ten = rowFor(applied.value(QStringLiteral("sections_retitled")).toArray(),
                                   QStringLiteral("from"), kWaveTen);
    EXPECT_EQ(ten.value(QStringLiteral("to")).toString(),
              QStringLiteral("ten wave (0.10.0 wave)"));

    QSet<QString> releaseSlugs;
    for (const char *key : {"sections_created", "sections_reused"})
        for (const QJsonValue &v : applied.value(QLatin1String(key)).toArray())
            releaseSlugs.insert(v.toObject().value(QStringLiteral("slug")).toString());
    const QSet<QString> released = {QStringLiteral("0.1.0"), QStringLiteral("0.2.0"),
                                    QStringLiteral("0.3.0"), QStringLiteral("0.9.0"),
                                    QStringLiteral("0.10.0")};
    auto store = openStore(RoadmapStore::Access::Interactive);
    ASSERT_NE(store, nullptr);
    const auto sections = store->listSectionsOrdered(fx.projectId);
    ASSERT_TRUE(sections.has_value());
    for (const auto &s : *sections) {
        if (s.level != 2) continue;
        const QString v = RoadmapIndex::versionOfTitle(s.title);
        if (v.isEmpty() || v.endsWith(QLatin1String(".x")) || !released.contains(v)) continue;
        EXPECT_TRUE(releaseSlugs.contains(s.slug))
            << s.title.toStdString() << " leads with a released version";
    }
}

// ----------------------------------------------------------------- INV-10 ---
TEST(RoadmapPlaceByRelease, Inv10NewSectionsStandInSemVerOrder) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Fixture fx = seed(guard, tmp);
    ASSERT_FALSE(fx.root.isEmpty());

    ASSERT_VERB_OK(dryRunThenApply(fx));
    const QString md = QString::fromUtf8(readFile(fx.root + QStringLiteral("/ROADMAP.md")));
    QStringList order;
    static const QRegularExpression h2(QStringLiteral("^## (.*)$"),
                                       QRegularExpression::MultilineOption);
    for (auto it = h2.globalMatch(md); it.hasNext();) {
        const QString v = RoadmapIndex::versionOfTitle(it.next().captured(1));
        if (!v.isEmpty()) order << v;
    }
    EXPECT_EQ(order, (QStringList{QStringLiteral("0.2.0"), QStringLiteral("0.3.0"),
                                  QStringLiteral("0.9.0"), QStringLiteral("0.10.0"),
                                  QStringLiteral("1.0.0")}))
        << md.toStdString();
    EXPECT_TRUE(md.contains(QStringLiteral("## 0.9.0 — released 2026-09-01")))
        << md.toStdString();
}

// ----------------------------------------------------------------- INV-11 ---
TEST(RoadmapPlaceByRelease, Inv11APreReleaseTagNeverPlaces) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Fixture fx = seed(guard, tmp);
    ASSERT_FALSE(fx.root.isEmpty());

    const QJsonObject dry = dryRun(fx);
    ASSERT_VERB_OK(dry);
    const QJsonArray unplaced = dry.value(QStringLiteral("unplaced")).toArray();
    EXPECT_EQ(rowFor(unplaced, QStringLiteral("id"), QStringLiteral("DEMO-0004"))
                  .value(QStringLiteral("reason")).toString(), QStringLiteral("unreleased"))
        << dump(dry);
    EXPECT_EQ(rowFor(unplaced, QStringLiteral("id"), QStringLiteral("DEMO-0012"))
                  .value(QStringLiteral("reason")).toString(), QStringLiteral("unreleased"));
    for (const QJsonValue &v : dry.value(QStringLiteral("sections_created")).toArray())
        EXPECT_FALSE(v.toObject().value(QStringLiteral("title")).toString()
                         .contains(QStringLiteral("0.4.0"))) << dump(dry);
}

// ----------------------------------------------------------------- INV-12 ---
TEST(RoadmapPlaceByRelease, Inv12OneStampTwoRowsPerMove) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Fixture fx = seed(guard, tmp);
    ASSERT_FALSE(fx.root.isEmpty());

    const qint64 mark = maxHistoryId();
    ASSERT_GE(mark, 0);
    const QJsonObject applied = dryRunThenApply(fx);
    ASSERT_VERB_OK(applied);
    const int n = applied.value(QStringLiteral("moved_count")).toInt();
    ASSERT_GT(n, 0);
    EXPECT_EQ(historyCount(fx.projectId, QStringLiteral("section"), mark), n);
    EXPECT_EQ(historyCount(fx.projectId, QStringLiteral("placed_by"), mark), n);

    auto store = openStore(RoadmapStore::Access::Interactive);
    ASSERT_NE(store, nullptr);
    QSqlQuery q(RoadmapStoreTestAccess::db(*store));
    q.prepare(QStringLiteral(
        "SELECT COUNT(DISTINCT changed_at) FROM history "
        "WHERE history_id > ? AND field IN ('section', 'placed_by')"));
    q.addBindValue(mark);
    ASSERT_TRUE(q.exec() && q.next());
    EXPECT_EQ(q.value(0).toInt(), 1);
}
