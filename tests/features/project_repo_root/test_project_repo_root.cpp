// Feature-conformance test for ANTS-5667 — an outer project folder that is not
// a git repository resolves, through its .ants/project.json `repo_root`, to the
// clone one level down. Contract: docs/specs/ANTS-5667-outer-folder-repo-root.md
// (pointer: tests/features/project_repo_root/spec.md).
//
// Every store here is a throwaway: XdgGuard points XDG_DATA_HOME into the
// QTemporaryDir before anything resolves RoadmapStore::defaultPath().

#include "../../_support/srcgrep.h"
#include "../../_support/xdg_guard.h"

#include "projectsettings.h"
#include "remotecontrol.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapsource.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QTemporaryDir>

#include <memory>
#include <string>

#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif

// The rcdetail helpers this test drives. remotecontrol_internal.h is for the
// remotecontrol sources only (RcTuSplit INV-5), so they are declared here, as
// workspace_search_phrase_hint does; the linker checks the signatures.
namespace rcdetail {
QString findRoadmapUnder(const QString &canonicalRoot, QString *ownerDir);
QString rcProjectRootFor(const QString &path);
QString rlStoreCounterPrefix(RoadmapStore &store, qint64 projectId,
                             const QString &idPrefixArg,
                             RoadmapSource::RoadmapText &text,
                             const QString &callerCanonical, bool *guessed);
}  // namespace rcdetail

namespace {

// ants-v1, the dialect the store serves, so a markdown answer can only mean the
// lookup missed the store row.
const char *kRoadmap =
    "# Demo Roadmap\n"
    "\n"
    "## To Do\n"
    "\n"
    "- \xF0\x9F\x93\x8B [DEMO-0001] **A seed bullet.**\n"
    "  Layman: A thing.\n"
    "  Kind: chore.\n"
    "  Source: seed.\n";

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QString dump(const QJsonObject &o) {
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// NEVER default-construct RoadmapStore before XdgGuard has redirected
// XDG_DATA_HOME: defaultPath() is the developer's real store.
bool migrate(const QString &root) {
    auto store = std::make_unique<RoadmapStore>(
        RoadmapStore::defaultPath(), RoadmapStore::kDefaultHistoryCapBytes,
        RoadmapStore::Access::Bulk);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return false;
    }
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) {
        ADD_FAILURE() << "findRoadmaps: " << err.toStdString();
        return false;
    }
    const auto plan = RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"),
                                               QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-10-12T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) {
        ADD_FAILURE() << "migration load: " << out.error.toStdString();
        return false;
    }
    return true;
}

// The layout the user keeps: outer/ (no .git) and outer/clone/ (a checkout).
struct Layout {
    QString outer;   // canonical
    QString clone;   // canonical
};

// `withKey` false leaves the outer folder with no settings file (INV-8).
// `withRoadmap` false skips the clone's roadmap and migration.
Layout seed(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
            bool withKey = true, bool withRoadmap = true) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString outerRaw = QDir(tmp.path()).filePath(QStringLiteral("outer"));
    const QString cloneRaw = outerRaw + QStringLiteral("/clone");
    QDir().mkpath(cloneRaw + QStringLiteral("/.git"));
    if (withRoadmap) {
        writeFile(cloneRaw + QStringLiteral("/ROADMAP.md"), QByteArray(kRoadmap));
        writeFile(cloneRaw + QStringLiteral("/.roadmap-counter"), QByteArray("1\n"));
    }
    if (withKey)
        writeFile(outerRaw + QStringLiteral("/.ants/project.json"),
                  QByteArray("{\"repo_root\":\"clone\"}"));
    Layout l{QFileInfo(outerRaw).canonicalFilePath(),
             QFileInfo(cloneRaw).canonicalFilePath()};
    if (withRoadmap && !migrate(l.clone))
        l.clone.clear();
    return l;
}

QString srcText(const char *rel) {
    return QString::fromStdString(
        ants_test::slurpFile(std::string(ANTS_SOURCE_DIR) + "/" + rel));
}

}  // namespace

// ----------------------------------------------------------------- INV-2 ----
// findRoadmapUnder probes the repo root, not the folder, and owns the repo root
// — with or without a roadmap in the outer folder.
TEST(ProjectRepoRoot, FindRoadmapUnder) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Layout l = seed(guard, tmp, true, false);
    ASSERT_TRUE(writeFile(l.clone + QStringLiteral("/ROADMAP.md"), QByteArray(kRoadmap)));

    for (const bool outerHasOwn : {false, true}) {
        if (outerHasOwn)
            ASSERT_TRUE(writeFile(l.outer + QStringLiteral("/ROADMAP.md"),
                                  QByteArray("# Outer\n")));
        QString owner;
        const QString hit = rcdetail::findRoadmapUnder(l.outer, &owner);
        EXPECT_EQ(hit, l.clone + QStringLiteral("/ROADMAP.md"))
            << (outerHasOwn ? "outer roadmap present" : "key only");
        EXPECT_EQ(owner, l.clone)
            << "ownerDir is the store key; it must be the repo root";
    }
}

// ----------------------------------------------------------------- INV-3 ----
TEST(ProjectRepoRoot, RoadmapVerbsUseStore) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Layout l = seed(guard, tmp);
    ASSERT_FALSE(l.clone.isEmpty());

    EXPECT_EQ(rcdetail::rcProjectRootFor(l.outer), l.clone);

    QJsonObject q;
    q[QStringLiteral("caller_cwd")] = l.outer;
    RemoteControl reader(nullptr);
    const QJsonObject r = reader.cmdRoadmapQueryForTest(q).object();
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << dump(r).toStdString();
    EXPECT_EQ(r.value(QStringLiteral("source")).toString(), QStringLiteral("store"))
        << dump(r).toStdString();

    QJsonObject a;
    a[QStringLiteral("caller_cwd")] = l.outer;
    a[QStringLiteral("section")]    = QStringLiteral("to-do");
    a[QStringLiteral("status")]     = QStringLiteral("planned");
    a[QStringLiteral("headline")]   = QStringLiteral("An append from the outer folder.");
    a[QStringLiteral("layman")]     = QStringLiteral("A plain-language line.");
    a[QStringLiteral("kind")]       = QStringLiteral("chore");
    a[QStringLiteral("source")]     = QStringLiteral("ants-5667-test");
    RemoteControl writer(nullptr);
    const QJsonObject w = writer.cmdRoadmapLogAppendForTest(a).object();
    ASSERT_TRUE(w.value(QStringLiteral("ok")).toBool()) << dump(w).toStdString();
    EXPECT_FALSE(w.contains(QStringLiteral("line")))
        << "the append spliced a markdown file instead of the store: "
        << dump(w).toStdString();

    // Read back from the clone, on a fresh instance so no cache answers.
    QJsonObject back;
    back[QStringLiteral("caller_cwd")] = l.clone;
    back[QStringLiteral("query")]      = QStringLiteral("append from the outer folder");
    RemoteControl checker(nullptr);
    const QJsonObject b = checker.cmdRoadmapQueryForTest(back).object();
    ASSERT_TRUE(b.value(QStringLiteral("ok")).toBool()) << dump(b).toStdString();
    EXPECT_EQ(b.value(QStringLiteral("bullets")).toArray().size(), 1)
        << dump(b).toStdString();
}

// ----------------------------------------------------------------- INV-4 ----
TEST(ProjectRepoRoot, MailboxFromOuterFolder) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Layout l = seed(guard, tmp);
    ASSERT_FALSE(l.clone.isEmpty());

    RoadmapStore store(RoadmapStore::defaultPath());
    QString err, code;
    ASSERT_TRUE(store.open(&err)) << err.toStdString();
    const auto pid = store.projectIdForRoot(l.clone, &err);
    ASSERT_TRUE(pid.has_value()) << err.toStdString();
    qint64 mid = 0;
    ASSERT_TRUE(store.sendMessage(*pid, QStringLiteral("demo"),
                                  QStringLiteral("hello"), QString(),
                                  QStringLiteral("2026-10-12T10:00:00Z"),
                                  &mid, &code, &err))
        << code.toStdString() << ": " << err.toStdString();

    RemoteControl rc(nullptr);
    QJsonObject inbox;
    inbox[QStringLiteral("caller_cwd")] = l.outer;
    inbox[QStringLiteral("op")]         = QStringLiteral("inbox");
    const QJsonObject i = rc.cmdSessionMessage(inbox).object();
    ASSERT_TRUE(i.value(QStringLiteral("ok")).toBool()) << dump(i).toStdString();
    EXPECT_EQ(i.value(QStringLiteral("unacked_count")).toInt(), 1)
        << dump(i).toStdString();

    QJsonObject ack;
    ack[QStringLiteral("caller_cwd")] = l.outer;
    ack[QStringLiteral("op")]         = QStringLiteral("ack");
    ack[QStringLiteral("message_id")] = mid;
    const QJsonObject k = rc.cmdSessionMessage(ack).object();
    ASSERT_TRUE(k.value(QStringLiteral("ok")).toBool()) << dump(k).toStdString();

    QVector<RoadmapStore::Message> got;
    int unacked = -1;
    ASSERT_TRUE(store.inboxFor(*pid, false, 0, 0, &got, &unacked, &err))
        << err.toStdString();
    EXPECT_EQ(unacked, 0) << "the ack from the outer folder did not clear it "
                             "for the clone";
}

// ----------------------------------------------------------------- INV-5 ----
TEST(ProjectRepoRoot, MailPendingFromOuterFolder) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Layout l = seed(guard, tmp);
    ASSERT_FALSE(l.clone.isEmpty());
    {
        RoadmapStore store(RoadmapStore::defaultPath());
        QString err, code;
        ASSERT_TRUE(store.open(&err)) << err.toStdString();
        const auto pid = store.projectIdForRoot(l.clone, &err);
        ASSERT_TRUE(pid.has_value());
        qint64 mid = 0;
        ASSERT_TRUE(store.sendMessage(*pid, QStringLiteral("demo"),
                                      QStringLiteral("hello"), QString(),
                                      QStringLiteral("2026-10-12T10:00:00Z"),
                                      &mid, &code, &err));
    }
    // Control: the clone itself reports it.
    ASSERT_EQ(RemoteControl::buildMailPendingBlock(l.clone)
                  .value(QStringLiteral("unacked_count")).toInt(), 1);
    EXPECT_EQ(RemoteControl::buildMailPendingBlock(l.outer)
                  .value(QStringLiteral("unacked_count")).toInt(), 1)
        << "session_orient from the outer folder missed the clone's mail";
}

// INV-5's status-bar half: the chip runs in the terminal, so a source check.
TEST(ProjectRepoRoot, MailChipMapsThroughRepoRootOf) {
    const QString src = srcText("src/claudestatuswidgets.cpp");
    const QString body = QString::fromStdString(ants_test::slurpFunctionBody(
        src.toStdString(), "ClaudeStatusBarController::refreshMailChip"));
    ASSERT_FALSE(body.isEmpty());
    ASSERT_TRUE(body.contains(QStringLiteral("projectIdContaining(")));
    // The folder handed to the store lookup is the mapped one.
    static const QRegularExpression mapped(QStringLiteral(
        R"(projectIdContaining\(\s*ProjectSettings::repoRootOf\()"));
    EXPECT_TRUE(mapped.match(body).hasMatch())
        << "refreshMailChip must look up ProjectSettings::repoRootOf(cwd)";
}

// ----------------------------------------------------------------- INV-6 ----
TEST(ProjectRepoRoot, ButtonUsesFindRoadmapUnder) {
    const std::string mw = ants_test::slurpFile(
        std::string(ANTS_SOURCE_DIR) + "/src/mainwindow.cpp");
    const std::string body =
        ants_test::slurpFunctionBody(mw, "void MainWindow::refreshRoadmapButton");
    ASSERT_FALSE(body.empty());
    EXPECT_NE(body.find("RemoteControl::roadmapFileFor("), std::string::npos)
        << "the button must ask the same finder roadmap_query does";
    // …and that wrapper is the finder itself, not a second probe.
    const std::string wrap = ants_test::slurpFunctionBody(
        ants_test::slurpRemoteControl(), "QString RemoteControl::roadmapFileFor");
    EXPECT_NE(wrap.find("rcdetail::findRoadmapUnder("), std::string::npos);
    EXPECT_EQ(body.find("ROADMAP.md"), std::string::npos)
        << "the button still carries its own candidate list";
}

// ----------------------------------------------------------------- INV-7 ----
TEST(ProjectRepoRoot, ProjectSettingsKey) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Layout l = seed(guard, tmp, false, false);
    ASSERT_TRUE(writeFile(l.outer + QStringLiteral("/afile"), QByteArray("x\n")));
    RemoteControl rc(nullptr);

    // detect, on a folder with no settings file, never proposes the key.
    QJsonObject det;
    det[QStringLiteral("caller_cwd")] = l.outer;
    det[QStringLiteral("op")]         = QStringLiteral("detect");
    const QJsonObject d = rc.cmdProjectSettings(det).object();
    ASSERT_TRUE(d.value(QStringLiteral("ok")).toBool()) << dump(d).toStdString();
    EXPECT_FALSE(d.value(QStringLiteral("suggestion")).toObject()
                     .contains(QStringLiteral("repo_root")));

    const auto set = [&](const QString &value) {
        QJsonObject s;
        s[QStringLiteral("caller_cwd")] = l.outer;
        s[QStringLiteral("op")]         = QStringLiteral("set");
        s[QStringLiteral("repo_root")]  = value;
        return rc.cmdProjectSettings(s).object();
    };
    for (const QString &bad : {QStringLiteral("missing"), QStringLiteral("../x"),
                               QStringLiteral("afile")}) {
        const QJsonObject r = set(bad);
        EXPECT_EQ(r.value(QStringLiteral("code")).toString(), QStringLiteral("bad_path"))
            << bad.toStdString() << ": " << dump(r).toStdString();
    }

    const QJsonObject ok = set(QStringLiteral("clone"));
    ASSERT_TRUE(ok.value(QStringLiteral("ok")).toBool()) << dump(ok).toStdString();
    EXPECT_EQ(ProjectSettings::repoRootOf(l.outer), l.clone);

    QJsonObject get;
    get[QStringLiteral("caller_cwd")] = l.outer;
    get[QStringLiteral("op")]         = QStringLiteral("get");
    const QJsonObject g = rc.cmdProjectSettings(get).object();
    EXPECT_EQ(g.value(QStringLiteral("declared")).toObject()
                  .value(QStringLiteral("repo_root")).toString(),
              QStringLiteral("clone"))
        << dump(g).toStdString();
}

// ----------------------------------------------------------------- INV-8 ----
// Without the key nothing moves: these assert today's answers.
TEST(ProjectRepoRoot, NoKeyUnchanged) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Layout l = seed(guard, tmp, false, true);
    ASSERT_FALSE(l.clone.isEmpty());

    EXPECT_EQ(ProjectSettings::repoRootOf(l.clone), l.clone);
    EXPECT_EQ(ProjectSettings::repoRootOf(l.outer), l.outer);
    QString owner;
    EXPECT_EQ(rcdetail::findRoadmapUnder(l.clone, &owner),
              l.clone + QStringLiteral("/ROADMAP.md"));
    EXPECT_EQ(owner, l.clone);
    EXPECT_EQ(rcdetail::rcProjectRootFor(l.clone), l.clone);
    EXPECT_TRUE(rcdetail::findRoadmapUnder(l.outer, nullptr).isEmpty())
        << "a non-git folder with no key must not look into its children";

    RemoteControl rc(nullptr);
    QJsonObject inbox;
    inbox[QStringLiteral("caller_cwd")] = l.outer;
    inbox[QStringLiteral("op")]         = QStringLiteral("inbox");
    const QJsonObject i = rc.cmdSessionMessage(inbox).object();
    EXPECT_EQ(i.value(QStringLiteral("code")).toString(), QStringLiteral("no_project"))
        << dump(i).toStdString();
    inbox[QStringLiteral("caller_cwd")] = l.clone;
    EXPECT_TRUE(rc.cmdSessionMessage(inbox).object()
                    .value(QStringLiteral("ok")).toBool());
}

// ----------------------------------------------------------------- INV-9 ----
// With no declaration and no stored prefix the chain falls to the folder-name
// default, so the outer folder must hand over the clone's name.
TEST(ProjectRepoRoot, CounterPrefixFromOuterFolder) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Layout l = seed(guard, tmp, true, false);

    RoadmapStore store(RoadmapStore::defaultPath());
    QString err;
    ASSERT_TRUE(store.open(&err)) << err.toStdString();
    constexpr qint64 kNoRow = 999999;   // no id_prefix row for this id

    auto fromClone = RoadmapSource::RoadmapText::fromMemory(QString());
    auto fromOuter = RoadmapSource::RoadmapText::fromMemory(QString());
    const QString want = rcdetail::rlStoreCounterPrefix(
        store, kNoRow, QString(), fromClone, l.clone, nullptr);
    ASSERT_FALSE(want.isEmpty());
    EXPECT_EQ(rcdetail::rlStoreCounterPrefix(store, kNoRow, QString(),
                                             fromOuter, l.outer, nullptr),
              want)
        << "the outer folder would allocate under its own folder-name prefix";
}
