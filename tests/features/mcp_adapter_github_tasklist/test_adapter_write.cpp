// ANTS-1428 Tier 2 — write-side `op:"flip"` adapter for GFM-task-
// list roadmaps. Behavioural tests against
// `RemoteControl::cmdRoadmapLog` with a QTemporaryDir-built project
// root. RemoteControl is constructed with nullptr m_main; the flip
// path is m_main-independent.

#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

// Build a project root with the given ROADMAP.md content + optional
// counter value. Returns the canonical absolute path of the dir.
QString buildProject(QTemporaryDir &dir,
                     const QString &roadmapContent,
                     qint64 counter = 0) {
    const QString root = dir.path();
    {
        QFile f(root + QStringLiteral("/ROADMAP.md"));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return {};
        }
        f.write(roadmapContent.toUtf8());
    }
    if (counter > 0) {
        QFile cf(root + QStringLiteral("/.roadmap-counter"));
        if (!cf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return {};
        }
        cf.write((QString::number(counter) + QChar('\n')).toUtf8());
    }
    // canonicalFilePath wants an existing path; the dir exists.
    return QFileInfo(root).canonicalFilePath();
}

QString readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readAll());
}

qint64 readCounter(const QString &root) {
    const QString cp = root + QStringLiteral("/.roadmap-counter");
    if (!QFile::exists(cp)) return -1;
    const QString raw = readFile(cp).trimmed();
    bool ok = false;
    const qint64 v = raw.toLongLong(&ok);
    return ok ? v : -2;
}

QJsonObject flip(RemoteControl &rc, const QJsonObject &req) {
    QJsonObject full = req;
    full["op"] = QStringLiteral("flip");
    return rc.cmdRoadmapLog(full).object();
}

}  // namespace

// INV-6 — anchor injection on first flip + counter advance.
TEST(AdapterWriteFlip, Inv6AnchorInjectionOnFirstFlip) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString md = QStringLiteral(
        "# Vestige\n\n"
        "## Phase 11A\n"
        "- [ ] First headline goes here\n"
        "- [ ] Second headline goes here\n");
    const QString root = buildProject(dir, md, 0);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = root;
    req["headline"]   = QStringLiteral("First headline goes here");
    req["to_status"]  = QStringLiteral("in-progress");
    req["prefix_hint"] = QStringLiteral("VEST");
    const QJsonObject env = flip(rc, req);
    EXPECT_TRUE(env.value("ok").toBool()) << QJsonDocument(env).toJson().constData();
    EXPECT_TRUE(env.value("anchor_injected").toBool());
    EXPECT_EQ(env.value("anchor").toString(),
              QStringLiteral("vest-0001"));
    EXPECT_EQ(env.value("counter").toInt(), 1);

    // Inspect the file.
    const QString after = readFile(root + QStringLiteral("/ROADMAP.md"));
    EXPECT_TRUE(after.contains(
        QStringLiteral("- [ ] 🚧 First headline goes here ^vest-0001")))
        << after.toUtf8().constData();
    // Other bullet untouched.
    EXPECT_TRUE(after.contains(
        QStringLiteral("- [ ] Second headline goes here")));

    // Counter advanced to 1.
    EXPECT_EQ(readCounter(root), 1);
}

// INV-7 — subsequent flip located by anchor does NOT inject again.
TEST(AdapterWriteFlip, Inv7AnchorReuseDoesNotReinject) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString md = QStringLiteral(
        "## Phase 11A\n"
        "- [ ] 🚧 First headline ^vest-0001\n"
        "- [ ] Second headline\n");
    const QString root = buildProject(dir, md, 1);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = root;
    req["anchor"]     = QStringLiteral("vest-0001");
    req["to_status"]  = QStringLiteral("shipped");
    const QJsonObject env = flip(rc, req);
    EXPECT_TRUE(env.value("ok").toBool()) << QJsonDocument(env).toJson().constData();
    EXPECT_FALSE(env.value("anchor_injected").toBool());
    EXPECT_FALSE(env.contains("counter"));

    // Anchor still in place; status flipped to [x].
    const QString after = readFile(root + QStringLiteral("/ROADMAP.md"));
    EXPECT_TRUE(after.contains(
        QStringLiteral("- [x] First headline ^vest-0001")))
        << after.toUtf8().constData();

    // Counter unchanged.
    EXPECT_EQ(readCounter(root), 1);
}

// INV-8 — locator with zero matches refuses with bullet_not_found.
TEST(AdapterWriteFlip, Inv8BulletNotFound) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString md = QStringLiteral(
        "## Phase 11A\n"
        "- [ ] First headline\n"
        "- [ ] Second headline\n");
    const QString root = buildProject(dir, md, 0);

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = root;
    req["id"]         = QStringLiteral("Nonexistent");
    req["to_status"]  = QStringLiteral("shipped");
    const QJsonObject env = flip(rc, req);
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(),
              QStringLiteral("bullet_not_found"));
    EXPECT_EQ(env.value("matched").toInt(), 0);
    EXPECT_TRUE(env.contains("suggestions"));

    // ROADMAP unchanged + counter unchanged (no file at start).
    const QString after = readFile(root + QStringLiteral("/ROADMAP.md"));
    EXPECT_EQ(after, md);
    EXPECT_EQ(readCounter(root), -1);  // .roadmap-counter not created
}

// INV-8 — ambiguous headline match → bullet_ambiguous.
TEST(AdapterWriteFlip, Inv8BulletAmbiguous) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString md = QStringLiteral(
        "## Phase 11A\n"
        "- [ ] Same headline\n"
        "- [ ] Same headline\n");
    const QString root = buildProject(dir, md, 0);

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = root;
    req["headline"]   = QStringLiteral("Same headline");
    req["to_status"]  = QStringLiteral("shipped");
    const QJsonObject env = flip(rc, req);
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(),
              QStringLiteral("bullet_ambiguous"));
    EXPECT_EQ(env.value("matched").toInt(), 2);
    const QJsonArray suggestions =
        env.value("suggestions").toArray();
    EXPECT_GE(suggestions.size(), 1);
}

// INV-9 — flip by bold-ID does NOT consume the counter.
TEST(AdapterWriteFlip, Inv9BoldIdDoesNotConsumeCounter) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString md = QStringLiteral(
        "## Phase 11A\n"
        "- [ ] **Sh4.** Shader work goes here\n");
    const QString root = buildProject(dir, md, 5);

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = root;
    req["id"]         = QStringLiteral("Sh4");
    req["to_status"]  = QStringLiteral("shipped");
    const QJsonObject env = flip(rc, req);
    EXPECT_TRUE(env.value("ok").toBool()) << QJsonDocument(env).toJson().constData();
    EXPECT_FALSE(env.value("anchor_injected").toBool());

    // Counter unchanged.
    EXPECT_EQ(readCounter(root), 5);
    // Status flipped.
    const QString after = readFile(root + QStringLiteral("/ROADMAP.md"));
    EXPECT_TRUE(after.contains(
        QStringLiteral("- [x] **Sh4.** Shader work goes here")));
}

// INV-10 — prefix_hint default derivation from leaf-dir.
TEST(AdapterWriteFlip, Inv10PrefixHintDefaultLeafDir) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // Force a known leaf-dir name by creating a sub-dir.
    const QString sub = dir.path() + QStringLiteral("/Vestige");
    ASSERT_TRUE(QDir().mkpath(sub));
    const QString md = QStringLiteral(
        "## Section\n"
        "- [ ] Headline without bold-id\n");
    QFile f(sub + QStringLiteral("/ROADMAP.md"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(md.toUtf8());
    f.close();
    const QString root = QFileInfo(sub).canonicalFilePath();

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = root;
    req["headline"]   = QStringLiteral("Headline without bold-id");
    req["to_status"]  = QStringLiteral("in-progress");
    const QJsonObject env = flip(rc, req);
    EXPECT_TRUE(env.value("ok").toBool()) << QJsonDocument(env).toJson().constData();
    EXPECT_EQ(env.value("anchor").toString(),
              QStringLiteral("vest-0001"));
}

// INV-10 — invalid prefix_hint refuses with bad_op_combo.
TEST(AdapterWriteFlip, Inv10PrefixHintInvalid) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString md = QStringLiteral(
        "## Section\n"
        "- [ ] Headline\n");
    const QString root = buildProject(dir, md, 0);

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"]  = root;
    req["headline"]    = QStringLiteral("Headline");
    req["to_status"]   = QStringLiteral("shipped");
    req["prefix_hint"] = QStringLiteral("lower-case-bad");
    const QJsonObject env = flip(rc, req);
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(),
              QStringLiteral("bad_op_combo"));
}

// INV-11 — fenced-code refusal.
TEST(AdapterWriteFlip, Inv11FencedCodeRefusal) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString md = QStringLiteral(
        "## Section\n\n"
        "```\n"
        "- [ ] **Fake.** This is inside a code fence\n"
        "```\n");
    const QString root = buildProject(dir, md, 0);

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = root;
    req["id"]         = QStringLiteral("Fake");
    req["to_status"]  = QStringLiteral("shipped");
    const QJsonObject env = flip(rc, req);
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(),
              QStringLiteral("anchor_unsafe_context"));

    // File unchanged.
    const QString after = readFile(root + QStringLiteral("/ROADMAP.md"));
    EXPECT_EQ(after, md);
}

// INV-12 — id_hint under op:"flip" is bad_op_combo.
TEST(AdapterWriteFlip, Inv12IdHintRejected) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString md = QStringLiteral(
        "## Section\n"
        "- [ ] Headline\n");
    const QString root = buildProject(dir, md, 0);

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = root;
    req["headline"]   = QStringLiteral("Headline");
    req["to_status"]  = QStringLiteral("shipped");
    req["id_hint"]    = 42;
    const QJsonObject env = flip(rc, req);
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(),
              QStringLiteral("bad_op_combo"));
}

// INV-13 — headline + (id|anchor) is bad_op_combo.
TEST(AdapterWriteFlip, Inv13HeadlineWithIdRejected) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString md = QStringLiteral(
        "## Section\n"
        "- [ ] **Sh4.** Some headline\n");
    const QString root = buildProject(dir, md, 0);

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = root;
    req["id"]         = QStringLiteral("Sh4");
    req["headline"]   = QStringLiteral("Some headline");
    req["to_status"]  = QStringLiteral("shipped");
    const QJsonObject env = flip(rc, req);
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(),
              QStringLiteral("bad_op_combo"));
}

// Mode preservation — default op (no `op` field) goes to append
// path; refuses on missing fields rather than treating as flip.
TEST(AdapterWriteFlip, AppendPathStillRequiresAllAppendFields) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString md = QStringLiteral(
        "## Section\n"
        "- ✅ [ANTS-0001] **Existing native bullet**\n");
    const QString root = buildProject(dir, md, 1);

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = root;
    // No op field — defaults to append. But all append-required
    // fields are missing, so we expect a missing_field refusal (the
    // m_main guard fires first since append needs MainWindow but rc
    // was constructed nullptr; either way it must not succeed).
    const QJsonObject env = rc.cmdRoadmapLog(req).object();
    EXPECT_FALSE(env.value("ok").toBool());
}
