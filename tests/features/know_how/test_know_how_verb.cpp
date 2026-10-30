// Feature-conformance test for ANTS-6310 — know_how's handler and schema.
// Contract: docs/specs/ANTS-6310-know-how-store.md (pointer: spec.md here).
//
// Every store is a throwaway: XdgGuard points XDG_DATA_HOME into the
// QTemporaryDir before anything resolves a default path.

#include "../../_support/xdg_guard.h"

#include "knowhowstore.h"
#include "remotecontrol.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>

#include <utility>

#ifndef ANTS_SOURCE_DIR
#  error "ANTS_SOURCE_DIR compile definition required"
#endif

namespace {

std::string dump(const QJsonObject &o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

struct Sandbox {
    QTemporaryDir tmp;
    ants_test::XdgGuard guard;
    QString root;

    Sandbox() {
        guard.setEnv("XDG_DATA_HOME", QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    }
    // Registers a project; the slug goes in the store under the sandbox.
    qint64 registerProject(const QString &slug) {
        const QString raw = QDir(tmp.path()).filePath(slug);
        QDir().mkpath(raw);
        root = QDir(raw).canonicalPath();
        RoadmapStore store(RoadmapStore::defaultPath());
        QString err;
        EXPECT_TRUE(store.open(&err)) << err.toStdString();
        const auto id = store.registerProject(root, slug, slug, &err);
        EXPECT_TRUE(id.has_value()) << err.toStdString();
        return id.value_or(0);
    }
    QJsonObject call(const QString &cwd, const char *op, const QJsonObject &extra = {}) {
        QJsonObject req = extra;
        req[QStringLiteral("caller_cwd")] = cwd;
        req[QStringLiteral("op")] = QString::fromLatin1(op);
        RemoteControl rc(nullptr);
        return rc.cmdKnowHow(req).object();
    }
};

const QJsonObject kEntry{{QStringLiteral("topic"), QStringLiteral("t")},
                         {QStringLiteral("title"), QStringLiteral("T")},
                         {QStringLiteral("body"), QStringLiteral("B")}};

QString code(const QJsonObject &o) { return o.value(QStringLiteral("code")).toString(); }

// user_version and sqlite_master, leaving out SQLite's own statistics table.
QStringList roadmapShape() {
    const QString name = QStringLiteral("know_how_verb_roadmap_shape");
    QStringList shape;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(RoadmapStore::defaultPath());
        EXPECT_TRUE(db.open());
        QSqlQuery q(db);
        EXPECT_TRUE(q.exec(QStringLiteral("PRAGMA user_version")));
        if (q.next())
            shape << q.value(0).toString();
        EXPECT_TRUE(q.exec(QStringLiteral(
            "SELECT type, name, sql FROM sqlite_master WHERE name NOT LIKE 'sqlite_stat%'"
            " ORDER BY type, name")));
        while (q.next())
            shape << q.value(0).toString() + q.value(1).toString() + q.value(2).toString();
        q.finish();
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    return shape;
}

QString readSource(const char *rel) {
    QFile f(QStringLiteral(ANTS_SOURCE_DIR "/") + QString::fromLatin1(rel));
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

QString toolBlock() {
    const QString all = readSource("src/claudeintegration.cpp");
    const int start = all.indexOf(QStringLiteral("t[\"name\"] = \"know_how\";"));
    const int end = start < 0 ? -1 : all.indexOf(QStringLiteral("tools.append(t);"), start);
    return end < 0 ? QString() : all.mid(start, end - start);
}

bool declares(const QString &block, const QString &key) {
    return block.contains(QStringLiteral("props[\"%1\"]").arg(key))
           || block.contains(QStringLiteral("add(\"%1\",").arg(key));
}

}  // namespace

// INV-7 — only a registered project writes, and the author is its slug.
TEST(KnowHowVerb, OnlyARegisteredProjectWrites) {
    Sandbox s;
    const QString store = KnowHowStore::defaultPath();
    ASSERT_TRUE(store.startsWith(s.tmp.path())) << "the sandbox did not take";

    const QString stranger = QDir(s.tmp.path()).filePath(QStringLiteral("stranger"));
    QDir().mkpath(stranger);

    const QJsonObject refused = s.call(stranger, "add", kEntry);
    EXPECT_EQ(code(refused), QStringLiteral("no_project")) << dump(refused);
    EXPECT_FALSE(QFileInfo::exists(store)) << "a refused write created the store";

    const QJsonObject read = s.call(stranger, "find", {{QStringLiteral("query"), QStringLiteral("x")}});
    EXPECT_TRUE(read.value(QStringLiteral("ok")).toBool()) << dump(read);
    EXPECT_FALSE(QFileInfo::exists(store));

    const qint64 project = s.registerProject(QStringLiteral("demo"));
    ASSERT_GT(project, 0);
    const QString sub = s.root + QStringLiteral("/deep/er");
    QDir().mkpath(sub);
    const QJsonObject added = s.call(sub, "add", kEntry);
    ASSERT_TRUE(added.value(QStringLiteral("ok")).toBool()) << dump(added);
    const QJsonObject got = s.call(stranger, "get", {{QStringLiteral("topic"), QStringLiteral("t")}});
    EXPECT_EQ(got.value(QStringLiteral("author")).toString(), QStringLiteral("demo")) << dump(got);

    // A caller-supplied author is not an argument, and is not honoured.
    QJsonObject forged = kEntry;
    forged[QStringLiteral("topic")] = QStringLiteral("forged");
    forged[QStringLiteral("author")] = QStringLiteral("someone-else");
    ASSERT_TRUE(s.call(s.root, "add", forged).value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(s.call(s.root, "get", {{QStringLiteral("topic"), QStringLiteral("forged")}})
                  .value(QStringLiteral("author")).toString(), QStringLiteral("demo"));

    {
        RoadmapStore roadmap(RoadmapStore::defaultPath());
        QString err;
        ASSERT_TRUE(roadmap.open(&err)) << err.toStdString();
        RoadmapStore::DeregisterCounts counts;
        ASSERT_TRUE(roadmap.deregisterProject(project, QStringLiteral("2026-10-30T12:00:00Z"), &counts, &err))
            << err.toStdString();
    }
    QJsonObject later = kEntry;
    later[QStringLiteral("topic")] = QStringLiteral("later");
    const QJsonObject gone = s.call(s.root, "add", later);
    EXPECT_EQ(code(gone), QStringLiteral("no_project")) << dump(gone);
    EXPECT_EQ(code(s.call(s.root, "get", {{QStringLiteral("topic"), QStringLiteral("later")}})),
              QStringLiteral("not_found"));
}

// INV-7 — an outer folder declaring `repo_root` writes as its clone's project,
// as cmdSessionMessage resolves it (ANTS-5667).
TEST(KnowHowVerb, AnOuterFolderWritesAsItsClone) {
    Sandbox s;
    const QString raw = QDir(s.tmp.path()).filePath(QStringLiteral("outer"));
    QDir().mkpath(raw + QStringLiteral("/clone/.git"));
    const QString outer = QDir(raw).canonicalPath();
    const QString clone = outer + QStringLiteral("/clone");
    {
        RoadmapStore store(RoadmapStore::defaultPath());
        QString err;
        ASSERT_TRUE(store.open(&err)) << err.toStdString();
        ASSERT_TRUE(store.registerProject(clone, QStringLiteral("demo"), QStringLiteral("demo"), &err).has_value())
            << err.toStdString();
    }
    QDir().mkpath(outer + QStringLiteral("/.ants"));
    QFile settings(outer + QStringLiteral("/.ants/project.json"));
    ASSERT_TRUE(settings.open(QIODevice::WriteOnly));
    settings.write("{\"repo_root\": \"clone\"}");
    settings.close();

    const QJsonObject added = s.call(outer, "add", kEntry);
    ASSERT_TRUE(added.value(QStringLiteral("ok")).toBool()) << dump(added);
    EXPECT_EQ(s.call(outer, "get", {{QStringLiteral("topic"), QStringLiteral("t")}})
                  .value(QStringLiteral("author")).toString(), QStringLiteral("demo"));
}

TEST(KnowHowVerb, RefusesAnUnknownOpAndAnUnresolvedCwd) {
    Sandbox s;
    const QString here = QDir(s.tmp.path()).canonicalPath();
    const QJsonObject op = s.call(here, "delete");
    EXPECT_EQ(code(op), QStringLiteral("bad_args")) << dump(op);

    const QJsonObject nowhere = s.call(here + QStringLiteral("/no/such/dir"), "find",
                                       {{QStringLiteral("query"), QStringLiteral("x")}});
    EXPECT_FALSE(nowhere.value(QStringLiteral("ok")).toBool(true)) << dump(nowhere);

    // op defaults to find.
    RemoteControl rc(nullptr);
    const QJsonObject plain = rc.cmdKnowHow({{QStringLiteral("caller_cwd"), here},
                                             {QStringLiteral("query"), QStringLiteral("x")}}).object();
    EXPECT_EQ(plain.value(QStringLiteral("op")).toString(), QStringLiteral("find")) << dump(plain);
}

// INV-8 — the roadmap store is untouched.
TEST(KnowHowVerb, WritesLeaveTheRoadmapStoreAlone) {
    Sandbox s;
    ASSERT_GT(s.registerProject(QStringLiteral("demo")), 0);
    const QStringList before = roadmapShape();
    ASSERT_GT(before.size(), 1);
    const QJsonObject added = s.call(s.root, "add", kEntry);
    ASSERT_TRUE(added.value(QStringLiteral("ok")).toBool()) << dump(added);
    ASSERT_TRUE(QFileInfo::exists(KnowHowStore::defaultPath()));
    EXPECT_EQ(roadmapShape(), before);
}

// The declared-input rule (§ 7). Every argument the handler or the store reads
// is declared: no check common to every verb holds this, so this one does. The
// keys are scraped, so an op added later and left undeclared fails here.
TEST(KnowHowVerb, SchemaDeclaresEveryArgumentRead) {
    const QString block = toolBlock();
    ASSERT_FALSE(block.isEmpty()) << "know_how tool block not found";

    static const QRegularExpression read(
        QStringLiteral("(?:req|args)\\.(?:value|contains)\\(QStringLiteral\\(\"([a-z_]+)\"\\)"));
    QSet<QString> keys;
    for (const char *rel : {"src/remotecontrol_know_how.cpp", "src/knowhowstore.cpp"}) {
        const QStringList lines = readSource(rel).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            if (line.trimmed().startsWith(QLatin1String("//")))
                continue;
            auto it = read.globalMatch(line);
            while (it.hasNext())
                keys.insert(it.next().captured(1));
        }
    }
    ASSERT_GE(keys.size(), 8) << "the scrape found too little to mean anything";
    // Read through helpers, so the scrape above cannot see them.
    for (const char *key : {"title", "body", "tags", "expected_revision", "revision", "reason", "topic"})
        keys.insert(QString::fromLatin1(key));
    for (const QString &key : std::as_const(keys))
        EXPECT_TRUE(declares(block, key)) << "read but never declared: " << key.toStdString();

    for (const char *op : {"find", "get", "list", "add", "update", "retire", "purge"})
        EXPECT_TRUE(block.contains(QStringLiteral("e.append(QStringLiteral(\"%1\"))").arg(QString::fromLatin1(op))))
            << "op enum is missing " << op;
    EXPECT_TRUE(block.contains(QStringLiteral("schema[\"additionalProperties\"] = false")));

    // § 4.3 and § 4.2 — the two limits the description must state.
    EXPECT_TRUE(block.contains(QStringLiteral("known secret shapes only")));
    EXPECT_TRUE(block.contains(QStringLiteral("live file only")));

    const QString registry = readSource("src/mcptoolregistry.cpp");
    const int reg = registry.indexOf(QStringLiteral("registerToolProvider(\"know_how\""));
    ASSERT_GE(reg, 0);
    EXPECT_LT(registry.indexOf(QStringLiteral("registerProjectScopedVerbs")), reg);
    EXPECT_TRUE(readSource("src/claudeintegration.cpp").contains(QStringLiteral("toolName == QStringLiteral(\"know_how\")")));
}
