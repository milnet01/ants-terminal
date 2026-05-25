// ANTS-1513 — feature-conformance test for TestAuditEngine::recheck.
// Behavioural: seeds a temp project (ROADMAP.md + cited files) and
// drives recheck() directly.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

#include "testauditengine.h"

namespace {

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

// A project root with a ROADMAP citing three findings + two of the cited
// files (the third is intentionally absent).
QString seedProject(const QString &root) {
    writeFile(QDir(root).filePath(QStringLiteral("ROADMAP.md")), QByteArray(
        "# Roadmap\n\n"
        "## Items\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-1234] **Flaky sleep.** see "
        "tests/test_driver.py:3\n"
        "  Source: audit.\n"
        "- \xF0\x9F\x93\x8B [ANTS-5678] **Clean cite.** see "
        "tests/test_clean.py:2\n"
        "  Source: audit.\n"
        "- \xF0\x9F\x93\x8B [ANTS-9000] **Gone file.** see "
        "tests/gone.py:5\n"
        "  Source: audit.\n"));
    // line 3 trips the sleep_call pre-pass pattern.
    writeFile(QDir(root).filePath(QStringLiteral("tests/test_driver.py")),
        QByteArray("import time\ndef test_x():\n    time.sleep(0.2)\n"));
    // line 2 has no smell.
    writeFile(QDir(root).filePath(QStringLiteral("tests/test_clean.py")),
        QByteArray("def test_y():\n    assert x == 1\n"));
    return root;
}

TestAuditEngine::RecheckResult recheck(const QString &root,
                                       const QString &id) {
    TestAuditEngine::RecheckRequest req;
    req.callerCwd = root;
    req.findingId = id;
    return TestAuditEngine::recheck(req);
}

}  // namespace

// INV-1 — live cite whose line still trips the smell.
TEST(test_audit_recheck, Inv1LiveCiteStillMatches) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    seedProject(tmp.path());
    const auto r = recheck(tmp.path(), QStringLiteral("ANTS-1234"));
    EXPECT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.found);
    EXPECT_EQ(r.citedFile, QStringLiteral("tests/test_driver.py"));
    EXPECT_EQ(r.citedLine, 3);
    EXPECT_TRUE(r.fileExists);
    EXPECT_TRUE(r.lineExists);
    EXPECT_TRUE(r.lineStillMatchesPattern);
    EXPECT_EQ(r.matchedPatternId, QStringLiteral("sleep_call"));
    EXPECT_EQ(r.matchedDimension, QStringLiteral("flakiness"));
}

// INV-2 — live cite whose line no longer trips any pattern.
TEST(test_audit_recheck, Inv2LiveCiteNoSmell) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    seedProject(tmp.path());
    const auto r = recheck(tmp.path(), QStringLiteral("ANTS-5678"));
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.found);
    EXPECT_TRUE(r.fileExists);
    EXPECT_TRUE(r.lineExists);
    EXPECT_FALSE(r.lineStillMatchesPattern);
    EXPECT_TRUE(r.matchedPatternId.isEmpty());
}

// INV-3 — cited file is gone.
TEST(test_audit_recheck, Inv3MissingFile) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    seedProject(tmp.path());
    const auto r = recheck(tmp.path(), QStringLiteral("ANTS-9000"));
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.found);
    EXPECT_EQ(r.citedFile, QStringLiteral("tests/gone.py"));
    EXPECT_FALSE(r.fileExists);
    EXPECT_FALSE(r.lineStillMatchesPattern);
}

// INV-4 — unknown id is found:false, not an error.
TEST(test_audit_recheck, Inv4UnknownId) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    seedProject(tmp.path());
    const auto r = recheck(tmp.path(), QStringLiteral("ANTS-0000"));
    EXPECT_TRUE(r.ok);
    EXPECT_FALSE(r.found);
}

// INV-5 — refusals: empty id, unresolvable cwd.
TEST(test_audit_recheck, Inv5Refusals) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    seedProject(tmp.path());
    const auto empty = recheck(tmp.path(), QString());
    EXPECT_FALSE(empty.ok);
    EXPECT_EQ(empty.code, QStringLiteral("missing_field"));

    const auto noProj = recheck(
        QStringLiteral("/no/such/dir/ants1513"), QStringLiteral("ANTS-1234"));
    EXPECT_FALSE(noProj.ok);
    EXPECT_EQ(noProj.code, QStringLiteral("no_project"));
}

// INV-6 — a cite that resolves outside the project root is never read.
TEST(test_audit_recheck, Inv6PathConfinement) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    // Project is a SUBDIR; the secret lives outside it but inside tmp.
    const QString proj = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    QDir().mkpath(proj);
    ASSERT_TRUE(writeFile(QDir(tmp.path()).filePath(QStringLiteral("outside.py")),
        QByteArray("import time\ntime.sleep(9)\n")));
    // ROADMAP cite escapes via `..` to the outside file (which exists +
    // would trip a pattern) — confinement must refuse to read it.
    ASSERT_TRUE(writeFile(QDir(proj).filePath(QStringLiteral("ROADMAP.md")),
        QByteArray("# R\n\n## Items\n\n"
                   "- \xF0\x9F\x93\x8B [ANTS-1] **Escape.** see "
                   "x/../../outside.py:2\n  Source: audit.\n")));
    const auto r = recheck(proj, QStringLiteral("ANTS-1"));
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.found);
    EXPECT_FALSE(r.fileExists)
        << "a cite resolving outside the project root must not be read";
    EXPECT_FALSE(r.lineStillMatchesPattern);
}
