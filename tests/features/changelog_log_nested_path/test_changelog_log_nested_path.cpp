// ANTS-4812 — feature-conformance test for changelog_log / changelog_query's
// optional `path`.
//
// The measured gap: a project that ships a second changelog for a
// separately-versioned bundled component could not reach it. Discovery walks
// UP from caller_cwd to the repo boundary, so a nested caller_cwd resolves back
// to the root file, and the nested one's only route was a raw edit — losing the
// atomic write, category routing, format validation and the [Unreleased] guard
// on exactly the file most likely to drift.
//
// Behavioural: drives RemoteControl::cmdChangelogLog / cmdChangelogQuery
// (both m_main-independent) over a temp project carrying two changelogs.
// Mirrors the changelog_log_normalize harness.

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

#include <string>

ANTS_TEST_SCOPE();

namespace {

const char *kSeed =
    "# Changelog\n\n"
    "## [Unreleased]\n\n"
    "### Added\n\n"
    "- **Seed entry.** (ANTS-0001)\n\n"
    "## [0.1.0] - 2026-01-01\n\n"
    "- old.\n";

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

std::string readFileStd(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll().toStdString();
}

bool contains(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}

QString rootCl(const QString &root) {
    return QDir(root).filePath(QStringLiteral("CHANGELOG.md"));
}
QString nestedRel() {
    return QStringLiteral("tools/audit/CHANGELOG.md");
}
QString nestedCl(const QString &root) {
    return QDir(root).filePath(nestedRel());
}

// A project with a root changelog AND a nested component changelog.
bool seedProject(const QString &root) {
    if (!QDir(root).mkpath(QStringLiteral("tools/audit"))) return false;
    if (!writeFile(rootCl(root), kSeed)) return false;
    return writeFile(nestedCl(root), kSeed);
}

QJsonObject addReq(const QString &root, const QString &summary) {
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    r[QStringLiteral("op")]         = QStringLiteral("add");
    r[QStringLiteral("category")]   = QStringLiteral("Fixed");
    r[QStringLiteral("summary")]    = summary;
    return r;
}

}  // namespace

// INV-1 — `path` writes to the nested changelog, and the root one is not
// touched. Without this the nested file was unreachable through the verb.
TEST(changelog_log_nested_path, Inv1PathWritesNestedNotRoot) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seedProject(tmp.path()));

    QJsonObject req = addReq(tmp.path(), QStringLiteral("Nested only."));
    req[QStringLiteral("path")] = nestedRel();
    const QJsonObject resp = RemoteControl(nullptr).cmdChangelogLog(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "changelog_log with an explicit path should write: "
        << resp.value(QStringLiteral("error")).toString().toStdString();

    EXPECT_TRUE(contains(readFileStd(nestedCl(tmp.path())), "Nested only."))
        << "the entry must land in the nested changelog named by `path`";
    EXPECT_FALSE(contains(readFileStd(rootCl(tmp.path())), "Nested only."))
        << "the root changelog must be untouched — writing there is the bug "
           "this argument exists to fix";
}

// INV-2 — changelog_query takes the same `path`, or the read side cannot check
// what the write side just wrote.
//
// The two files are seeded with DIFFERENT marker entries on purpose. Asserting
// only that the query sees the new entry passes vacuously against the pre-fix
// verb: there both the write and the read fall back to the root file, so the
// round-trip succeeds while reading the wrong changelog entirely. The marker
// pair is what makes this test measure the routing rather than the round-trip.
TEST(changelog_log_nested_path, Inv2QueryReadsTheSameNestedFile) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seedProject(tmp.path()));
    // Distinguish the two files.
    ASSERT_TRUE(writeFile(rootCl(tmp.path()),
        QByteArray(kSeed).replace("Seed entry.", "ROOT MARKER.")));
    ASSERT_TRUE(writeFile(nestedCl(tmp.path()),
        QByteArray(kSeed).replace("Seed entry.", "NESTED MARKER.")));

    QJsonObject wreq = addReq(tmp.path(), QStringLiteral("Readback probe."));
    wreq[QStringLiteral("path")] = nestedRel();
    ASSERT_TRUE(RemoteControl(nullptr).cmdChangelogLog(wreq)
                    .object().value(QStringLiteral("ok")).toBool());

    QJsonObject qreq;
    qreq[QStringLiteral("caller_cwd")] = tmp.path();
    qreq[QStringLiteral("path")]       = nestedRel();
    const QJsonObject qresp =
        RemoteControl(nullptr).cmdChangelogQuery(qreq).object();

    ASSERT_TRUE(qresp.value(QStringLiteral("ok")).toBool())
        << qresp.value(QStringLiteral("error")).toString().toStdString();
    const std::string body = QJsonDocument(qresp).toJson().toStdString();
    EXPECT_TRUE(contains(body, "Readback probe."))
        << "changelog_query with the same path must see the write";
    EXPECT_TRUE(contains(body, "NESTED MARKER."))
        << "the query must be reading the NESTED changelog";
    EXPECT_FALSE(contains(body, "ROOT MARKER."))
        << "the query read the root changelog — `path` was ignored on the "
           "read side, so it cannot check what the write side just wrote";
}

// INV-3 — a root-escaping path refuses bad_path and writes nothing.
TEST(changelog_log_nested_path, Inv3RootEscapingPathRefuses) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seedProject(tmp.path()));
    const std::string before = readFileStd(rootCl(tmp.path()));

    QJsonObject req = addReq(tmp.path(), QStringLiteral("Escapee."));
    req[QStringLiteral("path")] = QStringLiteral("../outside/CHANGELOG.md");
    const QJsonObject resp = RemoteControl(nullptr).cmdChangelogLog(req).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_path"))
        << "a path leaving the project root must refuse bad_path";
    EXPECT_EQ(readFileStd(rootCl(tmp.path())), before)
        << "a refused write must leave the root changelog byte-identical";
}

// INV-4 — `path` names an EXISTING changelog and never creates one.
TEST(changelog_log_nested_path, Inv4NonExistentPathRefusesAndCreatesNothing) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seedProject(tmp.path()));

    QJsonObject req = addReq(tmp.path(), QStringLiteral("Ghost."));
    req[QStringLiteral("path")] = QStringLiteral("docs/NOPE.md");
    const QJsonObject resp = RemoteControl(nullptr).cmdChangelogLog(req).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("no_changelog"));
    EXPECT_FALSE(QFile::exists(QDir(tmp.path()).filePath(
        QStringLiteral("docs/NOPE.md"))))
        << "the verb must not create a changelog it was pointed at";
}

// INV-5 — an absent `path` is unchanged: discovery still finds the root file.
// This is what keeps every existing caller byte-identical.
TEST(changelog_log_nested_path, Inv5AbsentPathStillWritesRoot) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seedProject(tmp.path()));

    const QJsonObject resp = RemoteControl(nullptr)
        .cmdChangelogLog(addReq(tmp.path(), QStringLiteral("Root default.")))
        .object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(contains(readFileStd(rootCl(tmp.path())), "Root default."))
        << "with no path, discovery must still resolve the root changelog";
    EXPECT_FALSE(contains(readFileStd(nestedCl(tmp.path())), "Root default."))
        << "the nested changelog must not absorb a default-routed write";
}
