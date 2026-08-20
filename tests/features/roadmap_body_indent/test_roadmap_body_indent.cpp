// ANTS-4554 / ANTS-4558 / ANTS-4557 — a bullet body keeps its shape.
// Contract: tests/features/roadmap_body_indent/spec.md
//
// Behavioural, on both backends: a fixture whose body carries a nested
// sub-bullet at four spaces and an indented command line, read back through
// roadmap_query and re-rendered through roadmap_log.

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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

#include <memory>
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

// DEMO-0007's body is the subject: a paragraph at the format's own two-space
// continuation indent, a sub-bullet nested at FOUR, and a command line
// indented deeper still.
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
        "  The steps, and their shape is the point:\n"
        "    - a nested sub-bullet at four spaces\n"
        "    - a second one\n"
        "  Run it with:\n"
        "      cmake --build build --target demo\n"
        "  Kind: implement.\n"
        "  Source: seed.\n"
        "\n"
        "- \xE2\x9C\x85 [DEMO-0003] **A shipped item.**\n"
        "  Layman: Another thing.\n"
        "  Kind: fix.\n"
        "  Source: seed.\n"
        "\n";
    return b;
}

QString seedFile(ants_test::XdgGuard &guard, const QTemporaryDir &tmp) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), fixture()))
        return QString();
    return QFileInfo(rawRoot).canonicalFilePath();
}

// The same fixture, imported — so the two backends can be asked one question.
QString seedMigrated(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
                     qint64 *projectId) {
    const QString root = seedFile(guard, tmp);
    if (root.isEmpty()) return QString();

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

QString bodyOf(RemoteControl &rc, const QString &root, const QString &id) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")]   = root;
    req[QStringLiteral("id")]           = id;
    req[QStringLiteral("include_body")] = true;
    const QJsonObject resp = rc.cmdRoadmapQuery(req).object();
    const QJsonArray bullets = resp.value(QStringLiteral("bullets")).toArray();
    if (bullets.isEmpty()) {
        ADD_FAILURE() << "no bullet for " << id.toStdString() << ": "
                      << QJsonDocument(resp).toJson().toStdString();
        return QString();
    }
    return bullets.at(0).toObject().value(QStringLiteral("body")).toString();
}

}  // namespace

// ---------------------------------------------------------------- INV-1 -----

// The read path returns the body's own shape. Relative depth survives: the
// two-space continuation indent the FORMAT owns is removed, and everything
// deeper than it is kept.
TEST(RoadmapBodyIndent, Inv1MarkdownReadKeepsRelativeIndent) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedFile(guard, tmp);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const std::string body = bodyOf(rc, root, QStringLiteral("DEMO-0007")).toStdString();

    EXPECT_TRUE(has(body, "\n  - a nested sub-bullet at four spaces"))
        << "the nested bullet must keep its depth relative to the body: " << body;
    EXPECT_TRUE(has(body, "\n    cmake --build build --target demo"))
        << "the command line must keep its deeper indent: " << body;
    EXPECT_TRUE(has(body, "The steps, and their shape is the point:"));
}

// ---------------------------------------------------------------- INV-2 -----

// And the head line is not part of the body. It is returned as `headline` and
// `headline_oneline` in the same envelope; a third copy is paid for on every
// fetch and read as the body's first line.
TEST(RoadmapBodyIndent, Inv2BodyExcludesTheHeadLine) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedFile(guard, tmp);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const QString bodyQ = bodyOf(rc, root, QStringLiteral("DEMO-0007"));
    const std::string body = bodyQ.toStdString();

    EXPECT_FALSE(has(body, "[DEMO-0007]")) << body;
    EXPECT_FALSE(has(body, "**An open item.**")) << body;
    EXPECT_TRUE(bodyQ.startsWith(QStringLiteral("Layman: A thing.")))
        << "the body begins at the first continuation line: " << body;
}

// ---------------------------------------------------------------- INV-3 -----

// One rule, both backends. A migrated project answers the same question the
// same way — the record is built by rendering the item and re-parsing it, so
// this is the round trip as well as the read.
TEST(RoadmapBodyIndent, Inv3StoreReadMatchesMarkdownRead) {
    QString migrated, plain;
    {
        ants_test::XdgGuard guard;
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        qint64 projectId = 0;
        const QString root = seedMigrated(guard, tmp, &projectId);
        ASSERT_FALSE(root.isEmpty());
        RemoteControl rc(nullptr);
        migrated = bodyOf(rc, root, QStringLiteral("DEMO-0007"));
    }
    {
        ants_test::XdgGuard guard;
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString root = seedFile(guard, tmp);
        ASSERT_FALSE(root.isEmpty());
        RemoteControl rc(nullptr);
        plain = bodyOf(rc, root, QStringLiteral("DEMO-0007"));
    }
    EXPECT_EQ(migrated, plain)
        << "store: [" << migrated.toStdString() << "]\nfile:  ["
        << plain.toStdString() << "]";
}

// ---------------------------------------------------------------- INV-4 -----

// The write path preserves depth too: a re-render must not re-indent a nested
// sub-bullet to the parent's level, which in Markdown re-parents it.
TEST(RoadmapBodyIndent, Inv4RerenderKeepsNestedDepth) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    // Any op re-renders the whole file (ANTS-3809 INV-2).
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("annotate");
    req[QStringLiteral("id")]         = QStringLiteral("DEMO-0003");
    req[QStringLiteral("note")]       = QStringLiteral("Progress: an unrelated write.");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    const std::string md =
        readAll(QDir(root).filePath(QStringLiteral("ROADMAP.md"))).toStdString();
    EXPECT_TRUE(has(md, "\n    - a nested sub-bullet at four spaces"))
        << "the re-render flattened a nested sub-bullet into the parent";
    EXPECT_TRUE(has(md, "\n      cmake --build build --target demo"))
        << "the re-render flattened an indented command line";
}

// ---------------------------------------------------------------- INV-5 -----

// What this deliberately does NOT do: strip the trailer lines. They are
// continuation lines of the bullet on both backends, and `Source:` /
// `Layman:` text is carried by no other field the list emits — so removing
// them would make the keyword filter blind to it.
TEST(RoadmapBodyIndent, Inv5TrailerLinesStayInTheBody) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedFile(guard, tmp);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const std::string body = bodyOf(rc, root, QStringLiteral("DEMO-0007")).toStdString();
    EXPECT_TRUE(has(body, "Kind: implement."));
    EXPECT_TRUE(has(body, "Source: seed."));
}

// ---------------------------------------------------------------- INV-6 -----

// And a trailer line the body never carried is COMPOSED into it, from the
// column, whenever the stored prose does not declare that key at a line start.
// INV-5's fixture cannot see this: DEMO-0007 declares `Kind:` and `Source:`
// itself, so the render suppresses both and every trailer in that body is the
// body's own.
//
// This is the render's contract (INV-12), not a leak: `body` answers "what
// does this bullet say in ROADMAP.md", and ROADMAP.md is generated from the
// store, so the line is really there. It is asserted because the question it
// does NOT answer is the one a reader assumes it does — "does the stored body
// declare this field?" — and a composed line makes an item whose column and
// body disagree look self-consistent. ANTS-4599.
TEST(RoadmapBodyIndent, Inv6ComposesATrailerTheBodyDoesNotDeclare) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    // The body names no trailer key at all; `lanes` reaches the column only
    // through the argument, which is the ordinary way the two diverge.
    const QString prose =
        QStringLiteral("A note whose prose never names that key.");
    ASSERT_FALSE(prose.contains(QStringLiteral("Lanes")));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("section")]    = QStringLiteral("work");
    req[QStringLiteral("status")]     = QStringLiteral("planned");
    req[QStringLiteral("headline")]   = QStringLiteral("An item whose lanes live only in the column.");
    req[QStringLiteral("kind")]       = QStringLiteral("implement");
    req[QStringLiteral("source")]     = QStringLiteral("seed");
    req[QStringLiteral("body")]       = prose;
    // The render gate refuses an open item with no `Layman:` line, and this
    // one's prose declares neither key — so `layman` is composed too, and by
    // the same rule.
    req[QStringLiteral("layman")]     = QStringLiteral("A plain-language line.");
    req[QStringLiteral("lanes")]      = QJsonArray{QStringLiteral("alpha"),
                                                   QStringLiteral("beta")};
    const QJsonObject resp = rc.cmdRoadmapLogAppendForTest(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const QString id = resp.value(QStringLiteral("id")).toString();
    ASSERT_FALSE(id.isEmpty()) << QJsonDocument(resp).toJson().toStdString();

    const std::string body = bodyOf(rc, root, id).toStdString();
    EXPECT_TRUE(has(body, "A note whose prose never names that key."))
        << "the submitted prose is missing from the body";
    EXPECT_TRUE(has(body, "Lanes: alpha, beta."))
        << "the read dropped a trailer line the render composes; body was:\n"
        << body;

    // The same line is in the generated file, which is why the markdown
    // backend answers this fetch the same way (INV-3): a walk of that file
    // reads the composed line as an ordinary continuation line.
    const std::string md =
        readAll(QDir(root).filePath(QStringLiteral("ROADMAP.md"))).toStdString();
    EXPECT_TRUE(has(md, "\n  Lanes: alpha, beta."))
        << "the re-render did not emit the composed trailer line";
}
