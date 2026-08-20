// ANTS-4576 / ANTS-4548 — a body that stops declaring a trailer key.
// Contract: tests/features/roadmap_log_trailer_undeclare/spec.md
//
// Behavioural, through roadmap_log itself: every case migrates a small
// markdown fixture into a store at RoadmapStore::defaultPath() (redirected
// into the case's sandbox), drives cmdRoadmapLogAmendBodyForTest, and then
// re-opens the store to assert what actually landed. Migrating rather than
// hand-building is what puts the DECLARATIONS in the body column, which is
// the state this whole item is about.

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
// REAL store under XDG_DATA_HOME. Every case redirects XDG_DATA_HOME first.
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

// Both bullets END on a prose line, deliberately: ANTS-4506 strips a TRAILING
// run of trailer-only lines out of the stored body, and a body with no
// declaration left in it cannot exercise anything here. With prose last, every
// declaration below stays in the body column — the shape the reproducer hit.
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
        "- \xF0\x9F\x93\x8B [DEMO-0007] **An open item.**\n"
        "  Layman: A thing.\n"
        "  Kind: implement.\n"
        "  Source: seed.\n"
        "  Closing prose line.\n"
        "\n"
        "- \xE2\x9C\x85 [DEMO-0003] **A shipped item.**\n"
        "  Layman: Another thing.\n"
        "  Kind: fix.\n"
        "  Source: seed.\n"
        "  Lanes: vt, core.\n"
        "  Evidence: docs/one.md\n"
        "  Closing prose line.\n"
        "\n";
    return b;
}

// findRoadmaps -> planFrom -> load, as the migration verb runs it. Bulk,
// because RoadmapMigrateLoad::load() refuses an Interactive connection.
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
    req[QStringLiteral("op")]         = QStringLiteral("amend_body");
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

}  // namespace

// ---------------------------------------------------------------- INV-1 -----

// Deleting a body's `Kind:` declaration succeeds — `kind` is NOT NULL, so
// "the body stopped saying it" cannot mean "it has none" — and the render
// re-emits the line canonically from the column it kept.
TEST(RoadmapLogTrailerUndeclare, Inv1KindDeclarationDeletedKeepsColumn) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogAmendBodyForTest(
              amendReq(root, QStringLiteral("DEMO-0007"),
                       QStringLiteral("Kind: implement."),
                       QStringLiteral("The kind is unchanged.")))
            .object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const auto item = itemOf(QStringLiteral("DEMO-0007"), projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->kind, QStringLiteral("implement"))
        << "an un-declared NOT NULL column keeps its value";
    EXPECT_TRUE(has(item->body.toStdString(), "The kind is unchanged."));
    EXPECT_TRUE(has(readAll(roadmapPath(root)).toStdString(), "Kind: implement."))
        << "the render re-emits the line canonically, so the file still declares it";
}

// ---------------------------------------------------------------- INV-2 -----

TEST(RoadmapLogTrailerUndeclare, Inv2SourceDeclarationDeletedKeepsColumn) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogAmendBodyForTest(
              amendReq(root, QStringLiteral("DEMO-0007"),
                       QStringLiteral("Source: seed."),
                       QStringLiteral("The source is unchanged.")))
            .object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const auto item = itemOf(QStringLiteral("DEMO-0007"), projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->source, QStringLiteral("seed"));
}

// ---------------------------------------------------------------- INV-3 -----

// `lanes` and `evidence` are NOT NULL DEFAULT '[]'. The empty list IS their
// absent state, so un-declaring empties them — and, unlike NULL, that write
// is one the schema accepts.
TEST(RoadmapLogTrailerUndeclare, Inv3LanesDeletedEmptiesToDefault) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogAmendBodyForTest(
              amendReq(root, QStringLiteral("DEMO-0003"),
                       QStringLiteral("Lanes: vt, core."),
                       QStringLiteral("The lanes are gone.")))
            .object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const auto item = itemOf(QStringLiteral("DEMO-0003"), projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_TRUE(item->lanes.isEmpty())
        << "lanes: " << item->lanes.join(QStringLiteral(",")).toStdString();
    EXPECT_FALSE(has(readAll(roadmapPath(root)).toStdString(), "Lanes: vt, core."));
}

TEST(RoadmapLogTrailerUndeclare, Inv3bEvidenceDeletedEmptiesToDefault) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogAmendBodyForTest(
              amendReq(root, QStringLiteral("DEMO-0003"),
                       QStringLiteral("Evidence: docs/one.md"),
                       QStringLiteral("The evidence is gone.")))
            .object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const auto item = itemOf(QStringLiteral("DEMO-0003"), projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_TRUE(item->evidence.isEmpty());
}

// ---------------------------------------------------------------- INV-4 -----

// `layman` is the one nullable column of the five, so it is the one that can
// genuinely be un-declared. On a SHIPPED item: the render's gate refuses an
// OPEN item with no Layman, which would refuse the write for a different and
// correct reason.
TEST(RoadmapLogTrailerUndeclare, Inv4LaymanDeletedClearsColumn) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogAmendBodyForTest(
              amendReq(root, QStringLiteral("DEMO-0003"),
                       QStringLiteral("Layman: Another thing."),
                       QStringLiteral("The plain-English line is gone.")))
            .object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const auto item = itemOf(QStringLiteral("DEMO-0003"), projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_TRUE(item->layman.isEmpty()) << item->layman.toStdString();
}

// ---------------------------------------------------------------- INV-5 -----

// No refusal on this path is a raw engine string. Whatever the outcome, an
// envelope that is not ok carries a documented code and never the words a
// SQLite constraint failure produces.
TEST(RoadmapLogTrailerUndeclare, Inv5NoRawConstraintStringEscapes) {
    for (const char *decl : {"Kind: implement.", "Source: seed.",
                             "Layman: A thing."}) {
        ants_test::XdgGuard guard;
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        qint64 projectId = 0;
        const QString root = seedMigrated(guard, tmp, &projectId);
        ASSERT_FALSE(root.isEmpty());

        RemoteControl rc(nullptr);
        const QJsonObject resp =
            rc.cmdRoadmapLogAmendBodyForTest(
                  amendReq(root, QStringLiteral("DEMO-0007"),
                           QString::fromLatin1(decl),
                           QStringLiteral("Replaced by prose.")))
                .object();
        const std::string err =
            resp.value(QStringLiteral("error")).toString().toStdString();
        EXPECT_FALSE(has(err, "constraint failed"))
            << "raw engine string for: " << decl << " -> " << err;
        EXPECT_FALSE(has(err, "Unable to fetch row")) << decl;
    }
}

// A DELIBERATE declaration whose value is outside `kind`'s closed vocabulary.
// The guard cannot catch this one — the shape is exactly what the render
// writes — so it reaches the column, and the column has a CHECK constraint.
TEST(RoadmapLogTrailerUndeclare, Inv5bUnrecognisedKindRefusesInWords) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogAmendBodyForTest(
              amendReq(root, QStringLiteral("DEMO-0007"),
                       QStringLiteral("Kind: implement."),
                       QStringLiteral("Kind: banana.")))
            .object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const std::string err =
        resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_FALSE(has(err, "constraint failed")) << err;
    EXPECT_TRUE(has(err, "banana")) << "the refusal quotes the rejected value: " << err;
    EXPECT_TRUE(has(err, "kind")) << err;
    const auto item = itemOf(QStringLiteral("DEMO-0007"), projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->kind, QStringLiteral("implement")) << "nothing written";
}

// The two shapes the PARSER recognises and the COLUMN would refuse: a capture
// arrives raw, so `Kind: Fix.` (case) and `Kind: bug.` (a § 7.4 alias) both
// reach the CHECK constraint unless § 2.6 canonicalises them as the migration
// does.
TEST(RoadmapLogTrailerUndeclare, Inv5cRecognisedKindIsCanonicalised) {
    struct Case { const char *written; const char *stored; };
    for (const Case &c : {Case{"Kind: Fix.", "fix"}, Case{"Kind: bug.", "fix"}}) {
        ants_test::XdgGuard guard;
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        qint64 projectId = 0;
        const QString root = seedMigrated(guard, tmp, &projectId);
        ASSERT_FALSE(root.isEmpty());

        RemoteControl rc(nullptr);
        const QJsonObject resp =
            rc.cmdRoadmapLogAmendBodyForTest(
                  amendReq(root, QStringLiteral("DEMO-0007"),
                           QStringLiteral("Kind: implement."),
                           QString::fromLatin1(c.written)))
                .object();
        EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
            << c.written << ": " << QJsonDocument(resp).toJson().toStdString();
        const auto item = itemOf(QStringLiteral("DEMO-0007"), projectId);
        ASSERT_TRUE(item.has_value());
        EXPECT_EQ(item->kind, QString::fromLatin1(c.stored)) << c.written;
    }
}

// ---------------------------------------------------------------- INV-6 -----

// ANTS-4548 — the preview shares the real write path (commitAndRender runs the
// mutation inside the transaction and rolls it back), so dry_run and the real
// call agree, refusal for refusal — and the dry run leaves the store and the
// file exactly as they were.
TEST(RoadmapLogTrailerUndeclare, Inv6DryRunAgreesWithTheRealCall) {
    struct Case { const char *id; const char *oldText; const char *newText; };
    const Case cases[] = {
        {"DEMO-0007", "Kind: implement.", "The kind is unchanged."},
        {"DEMO-0007", "Closing prose line.", "Prose naming Kind: mid-line."},
        {"DEMO-0007", "no such text in this body", "irrelevant"},
        {"DEMO-0007", "Kind: implement.", "Kind: banana."},
    };
    for (const Case &c : cases) {
        ants_test::XdgGuard guard;
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        qint64 projectId = 0;
        const QString root = seedMigrated(guard, tmp, &projectId);
        ASSERT_FALSE(root.isEmpty());
        const QByteArray fileBefore = readAll(roadmapPath(root));
        const auto itemBefore = itemOf(QString::fromLatin1(c.id), projectId);
        ASSERT_TRUE(itemBefore.has_value());

        RemoteControl rc(nullptr);
        QJsonObject req = amendReq(root, QString::fromLatin1(c.id),
                                   QString::fromLatin1(c.oldText),
                                   QString::fromLatin1(c.newText));
        req[QStringLiteral("dry_run")] = true;
        const QJsonObject preview = rc.cmdRoadmapLogAmendBodyForTest(req).object();

        EXPECT_EQ(readAll(roadmapPath(root)), fileBefore)
            << "INV-6: a preview writes no file (" << c.oldText << ")";
        const auto itemAfterPreview = itemOf(QString::fromLatin1(c.id), projectId);
        ASSERT_TRUE(itemAfterPreview.has_value());
        EXPECT_EQ(itemAfterPreview->body, itemBefore->body)
            << "INV-6: a preview writes no store row (" << c.oldText << ")";
        EXPECT_EQ(itemAfterPreview->kind, itemBefore->kind);

        req.remove(QStringLiteral("dry_run"));
        const QJsonObject real = rc.cmdRoadmapLogAmendBodyForTest(req).object();
        EXPECT_EQ(preview.value(QStringLiteral("ok")).toBool(),
                  real.value(QStringLiteral("ok")).toBool())
            << "preview and real call disagree on ok (" << c.oldText << "): "
            << QJsonDocument(preview).toJson().toStdString()
            << QJsonDocument(real).toJson().toStdString();
        EXPECT_EQ(preview.value(QStringLiteral("code")).toString(),
                  real.value(QStringLiteral("code")).toString())
            << "preview and real call disagree on code (" << c.oldText << ")";
    }
}

// ---------------------------------------------------------------- INV-7 -----

// `new_text` is caller prose, exactly as a `note` is. ANTS-4549 guards the
// note; this is the same guard on the argument it does not read.
TEST(RoadmapLogTrailerUndeclare, Inv7MidLineKeyInNewTextRefuses) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());
    const QByteArray before = readAll(roadmapPath(root));

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogAmendBodyForTest(
              amendReq(root, QStringLiteral("DEMO-0007"),
                       QStringLiteral("Closing prose line."),
                       QStringLiteral("This sentence names Kind: in running prose.")))
            .object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("body_shadowed"));
    const std::string err =
        (resp.value(QStringLiteral("error")).toString() +
         resp.value(QStringLiteral("hint")).toString()).toStdString();
    EXPECT_TRUE(has(err, "kind")) << err;
    EXPECT_TRUE(has(err, "new_text")) << "the refusal names the argument: " << err;
    EXPECT_EQ(readAll(roadmapPath(root)), before) << "nothing written";
}

// ---------------------------------------------------------------- INV-8 -----

// And a DELIBERATE declaration through new_text still works: label first on
// its line, as the render writes one. The guard is position, not vocabulary.
TEST(RoadmapLogTrailerUndeclare, Inv8DeliberateDeclarationInNewTextWrites) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogAmendBodyForTest(
              amendReq(root, QStringLiteral("DEMO-0007"),
                       QStringLiteral("Kind: implement."),
                       QStringLiteral("Kind: doc.")))
            .object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const auto item = itemOf(QStringLiteral("DEMO-0007"), projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->kind, QStringLiteral("doc"));
}
