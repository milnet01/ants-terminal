// ANTS-3601 — doc_integrity MCP verb conformance test. Behavioural INVs drive
// the pure RemoteControl helpers (docIntegrityEnumerate / …BuildResponse); the
// wiring INV (INV-10) source-scrapes the registration sites. The handler itself
// needs a live MainWindow, hence the pure-helper + source-scrape split (the
// rc_get_text_byte_cap pattern). See docs/specs/ANTS-3601.md § 2.6.

#include "remotecontrol.h"
#include "docintegrity.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#if !defined(ANTS_SOURCE_DIR) || !defined(SRC_MAINWINDOW_CPP_PATH) || \
    !defined(SRC_REMOTECONTROL_CPP_PATH) || !defined(SRC_CLAUDE_INTEGRATION_CPP_PATH)
#error "doc_integrity_verb test needs the test_claude source-path compile defs"
#endif

namespace {

bool writeFile(const QString &path, const QString &content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(content.toUtf8());
    return true;
}

QString slurp(const char *path) {
    QFile f(QString::fromUtf8(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

}  // namespace

// INV-16 — directory `path` scoping is exact; file → one doc; omitted →
// docs_dir walk; non-existent → empty.
TEST(DocIntegrityVerb, EnumerateScoping) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(writeFile(root + "/docs/a/one.md", "# H\n"));
    ASSERT_TRUE(writeFile(root + "/docs/a/sub/two.md", "# H\n"));
    ASSERT_TRUE(writeFile(root + "/docs/b/three.md", "# H\n"));
    ASSERT_TRUE(writeFile(root + "/README.md", "# H\n"));

    // Directory path → exactly the recursive *.md under it.
    const QStringList a = RemoteControl::docIntegrityEnumerate(root, "docs/a", "docs");
    EXPECT_EQ(a, (QStringList{"docs/a/one.md", "docs/a/sub/two.md"}));

    // File path → that one doc.
    EXPECT_EQ(RemoteControl::docIntegrityEnumerate(root, "docs/a/one.md", "docs"),
              (QStringList{"docs/a/one.md"}));

    // Omitted path → the docs_dir walk (docs/), NOT the root README.
    const QStringList all = RemoteControl::docIntegrityEnumerate(root, "", "docs");
    EXPECT_EQ(all, (QStringList{"docs/a/one.md", "docs/a/sub/two.md", "docs/b/three.md"}));
    EXPECT_FALSE(all.contains("README.md"));

    // Non-existent in-root path → empty (INV-15 at the verb layer).
    EXPECT_TRUE(RemoteControl::docIntegrityEnumerate(root, "docs/typo.md", "docs").isEmpty());
}

// INV-18 — the `kinds` filter narrows both findings AND counts.
TEST(DocIntegrityVerb, KindsFilterNarrowsCounts) {
    using DocIntegrity::Finding;
    using DocIntegrity::Kind;
    const QList<Finding> findings = {
        {Kind::DeadAnchor, "docs/a.md", 1, "m1"},
        {Kind::BrokenLink, "docs/a.md", 2, "m2"},
        {Kind::TocGap,     "docs/a.md", 3, "m3"},
    };

    // Unfiltered → all three kinds present.
    const QJsonObject all = RemoteControl::docIntegrityBuildResponse(
        findings, {}, {"docs/a.md"});
    EXPECT_EQ(all.value("findings").toArray().size(), 3);
    const QJsonObject allCounts = all.value("counts").toObject();
    EXPECT_EQ(allCounts.value("dead_anchor").toInt(), 1);
    EXPECT_EQ(allCounts.value("broken_link").toInt(), 1);
    EXPECT_EQ(allCounts.value("toc_gap").toInt(), 1);

    // Filtered to dead_anchor → findings + counts narrow together.
    const QJsonObject only = RemoteControl::docIntegrityBuildResponse(
        findings, QSet<QString>{"dead_anchor"}, {"docs/a.md"});
    const QJsonArray onlyFs = only.value("findings").toArray();
    ASSERT_EQ(onlyFs.size(), 1);
    EXPECT_EQ(onlyFs.at(0).toObject().value("kind").toString(), QStringLiteral("dead_anchor"));
    const QJsonObject onlyCounts = only.value("counts").toObject();
    EXPECT_EQ(onlyCounts.value("dead_anchor").toInt(), 1);
    EXPECT_FALSE(onlyCounts.contains("broken_link"));
    EXPECT_FALSE(onlyCounts.contains("toc_gap"));
}

// INV-10 — verb contract wiring: caller_cwd Required, path validation →
// bad_path, ETag-304. Source-scrape the registration sites.
TEST(DocIntegrityVerb, WiringRegistered) {
    const QString mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.isEmpty());
    // Registered with the Required caller_cwd contract.
    const int reg = mw.indexOf("registerToolProvider(\"doc_integrity\"");
    ASSERT_GE(reg, 0);
    EXPECT_TRUE(mw.mid(reg, 160).contains("CallerCwdContract::Required"));

    const QString ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.isEmpty());
    EXPECT_TRUE(ci.contains("\"doc_integrity\""));  // schema + registrations
    // callerCwdContractFor → Required for doc_integrity.
    const int cc = ci.indexOf("toolName == QStringLiteral(\"doc_integrity\")");
    ASSERT_GE(cc, 0);
    EXPECT_TRUE(ci.mid(cc, 100).contains("C::Required"));

    const QString rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.isEmpty());
    const int h = rc.indexOf("RemoteControl::cmdDocIntegrity");
    ASSERT_GE(h, 0);
    // The handler validates a supplied path and refuses bad_path on escape.
    const QString body = rc.mid(h, 1200);
    EXPECT_TRUE(body.contains("validatePath("));
    EXPECT_TRUE(body.contains("check.err"));
}
