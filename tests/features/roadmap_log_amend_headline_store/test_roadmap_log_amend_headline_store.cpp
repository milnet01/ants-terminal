// ANTS-4668 / ANTS-4683 — op:"amend_headline" on a STORE-BACKED project.
// Contract: tests/features/roadmap_log_amend_headline_store/spec.md
//
// Behavioural, through roadmap_log itself. Every case migrates a small
// markdown fixture into a store at RoadmapStore::defaultPath() (redirected
// into the case's sandbox), drives cmdRoadmapLogAmendHeadlineForTest, and
// re-opens the store to assert what landed in the `headline` COLUMN — the
// surface the op previously refused to touch at all.

#include "../../_support/expect.h"
#include "../../_support/xdg_guard.h"

#include "remotecontrol.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

#include <memory>
#include <optional>
#include <string>

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

QByteArray readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// NEVER default-construct RoadmapStore: defaultPath() resolves the developer's
// REAL machine-global store under XDG_DATA_HOME. Every case redirects
// XDG_DATA_HOME first, via XdgGuard.
std::unique_ptr<RoadmapStore> openStore(RoadmapStore::Access access) {
    auto store = std::make_unique<RoadmapStore>(
        RoadmapStore::defaultPath(), RoadmapStore::kDefaultHistoryCapBytes, access);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return nullptr;
    }
    return store;
}

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
    "totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi\n"
    "architecto beatae vitae dicta sunt explicabo. Nemo enim ipsam voluptatem\n"
    "quia voluptas sit aspernatur aut odit aut fugit, sed quia consequuntur\n"
    "magni dolores eos qui ratione voluptatem sequi nesciunt neque porro.\n"
    "Quisquam est, qui dolorem ipsum quia dolor sit amet, consectetur, adipisci\n"
    "velit, sed quia non numquam eius modi tempora incidunt ut labore.\n";

// DEMO-0007's headline says "fired twice" ONCE — the INV-1 target — and says
// "step" TWICE, which is INV-2's ambiguity without needing a second fixture.
// The wording deliberately echoes the reporter's own measured case (a headline
// asserting a claim its body later refutes), so the fixture reads as the
// defect rather than as an abstract string.
QByteArray fixture() {
    QByteArray b =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo \xE2\x80\x94 Roadmap\n"
        "\n";
    b += kPad;
    b += "\n"
        "## Work\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0007] **A failure fired twice, both times on "
        "step 1 of the step list.**\n"
        "  Layman: A thing.\n"
        "  Kind: implement.\n"
        "  Source: seed.\n"
        "  A third instance refuted the position claim outright.\n"
        "\n"
        "- \xE2\x9C\x85 [DEMO-0003] **A shipped item.**\n"
        "  Layman: Another thing.\n"
        "  Kind: fix.\n"
        "  Source: seed.\n"
        "  Closing prose line.\n"
        "\n";
    return b;
}

QString seedMigrated(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
                     qint64 *projectId) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());

    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), fixture()))
        return QString();
    // The store keys a project on its CANONICAL root, and /tmp is a symlink on
    // some hosts.
    const QString root = QFileInfo(rawRoot).canonicalFilePath();

    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return QString();
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) {
        ADD_FAILURE() << "findRoadmaps: " << err.toStdString();
        return QString();
    }
    const auto plan =
        RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"), QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-05T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) {
        ADD_FAILURE() << "migration load: " << out.error.toStdString();
        return QString();
    }
    *projectId = out.projectId;
    return root;
}

QJsonObject amendReq(const QString &root, const QString &id,
                     const QString &oldText, const QString &newText) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("amend_headline");
    req[QStringLiteral("id")]         = id;
    req[QStringLiteral("old_text")]   = oldText;
    req[QStringLiteral("new_text")]   = newText;
    return req;
}

std::optional<RoadmapStore::ItemWrite> itemOf(const QString &id, qint64 projectId) {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return std::nullopt;
    QString err;
    const auto pk = store->findItem(projectId, id, &err);
    if (!pk) {
        ADD_FAILURE() << "findItem " << id.toStdString() << ": " << err.toStdString();
        return std::nullopt;
    }
    return store->readItem(*pk, &err);
}

QString roadmapPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}

const char *kOriginalHeadline =
    "A failure fired twice, both times on step 1 of the step list.";

}  // namespace

// ---------------------------------------------------------------- INV-1 -----

TEST(RoadmapLogAmendHeadlineStore, Inv1StoreAmendWritesColumn) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogAmendHeadlineForTest(
              amendReq(root, QStringLiteral("DEMO-0007"),
                       QStringLiteral("fired twice"),
                       QStringLiteral("fired five times")))
            .object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("headline")).toString(),
              QStringLiteral("A failure fired five times, both times on "
                             "step 1 of the step list."))
        << "the success envelope echoes the new headline";

    const auto item = itemOf(QStringLiteral("DEMO-0007"), projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_TRUE(has(item->headline.toStdString(), "fired five times"))
        << "the store COLUMN carries the amendment, not just the file";
    EXPECT_FALSE(has(item->headline.toStdString(), "fired twice"));
    EXPECT_TRUE(has(readAll(roadmapPath(root)).toStdString(), "fired five times"))
        << "and the render published it";
}

// ---------------------------------------------------------------- INV-6 -----

// Separate from INV-1 because a refusal swapped for a DIFFERENT refusal would
// satisfy neither, and the regression this item exists for is the CODE.
TEST(RoadmapLogAmendHeadlineStore, Inv6NoUnsupportedFormatOnStoreProject) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogAmendHeadlineForTest(
              amendReq(root, QStringLiteral("DEMO-0007"),
                       QStringLiteral("fired twice"),
                       QStringLiteral("fired once")))
            .object();

    EXPECT_NE(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("unsupported_format"))
        << "a store-backed project must not be refused for being migrated";
}

// ---------------------------------------------------------------- INV-2 -----

TEST(RoadmapLogAmendHeadlineStore, Inv2AmbiguousRefusesAndWritesNothing) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    // "step" occurs twice in the headline.
    const QJsonObject resp =
        rc.cmdRoadmapLogAmendHeadlineForTest(
              amendReq(root, QStringLiteral("DEMO-0007"),
                       QStringLiteral("step"), QStringLiteral("stage")))
            .object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("headline_match_ambiguous"))
        << QJsonDocument(resp).toJson().toStdString();

    const auto item = itemOf(QStringLiteral("DEMO-0007"), projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->headline, QString::fromUtf8(kOriginalHeadline))
        << "an ambiguous match must not clobber the headline on a guess";
}

// ---------------------------------------------------------------- INV-3 -----

TEST(RoadmapLogAmendHeadlineStore, Inv3AbsentRefusesAndHintsAtBody) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    // Present in the BODY ("A third instance refuted the position claim
    // outright."), absent from the headline.
    const QJsonObject resp =
        rc.cmdRoadmapLogAmendHeadlineForTest(
              amendReq(root, QStringLiteral("DEMO-0007"),
                       QStringLiteral("A third instance"),
                       QStringLiteral("A fourth instance")))
            .object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("headline_match_not_found"))
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_TRUE(has(resp.value(QStringLiteral("hint")).toString().toStdString(),
                    "amend_body"))
        << "the caller is redirected rather than left concluding the text is absent";
}

// ---------------------------------------------------------------- INV-4 -----

TEST(RoadmapLogAmendHeadlineStore, Inv4EmptyHeadlineRefused) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogAmendHeadlineForTest(
              amendReq(root, QStringLiteral("DEMO-0007"),
                       QString::fromUtf8(kOriginalHeadline), QString()))
            .object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"))
        << QJsonDocument(resp).toJson().toStdString();

    const auto item = itemOf(QStringLiteral("DEMO-0007"), projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->headline, QString::fromUtf8(kOriginalHeadline))
        << "a bullet with no headline could not be located again";
}

// ---------------------------------------------------------------- INV-5 -----

TEST(RoadmapLogAmendHeadlineStore, Inv5DryRunWritesNothing) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    QJsonObject req = amendReq(root, QStringLiteral("DEMO-0007"),
                               QStringLiteral("fired twice"),
                               QStringLiteral("fired five times"));
    req[QStringLiteral("dry_run")] = true;
    const QJsonObject resp =
        rc.cmdRoadmapLogAmendHeadlineForTest(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_TRUE(resp.value(QStringLiteral("dry_run")).toBool());

    const auto item = itemOf(QStringLiteral("DEMO-0007"), projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->headline, QString::fromUtf8(kOriginalHeadline))
        << "a preview must not write the column";
    EXPECT_FALSE(has(readAll(roadmapPath(root)).toStdString(), "fired five times"))
        << "nor the file";
}
