// Feature-conformance test for ANTS-1111 RoadmapFoldIn helpers +
// AuditEngine::templateRoadmapFoldInBlock.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "auditengine.h"
#include "roadmapfoldin.h"

namespace {

// Write `body` to `<dir>/<name>` and return absolute path.
QString writeFile(const QString &dir, const QString &name,
                  const QByteArray &body) {
    const QString p = QDir(dir).filePath(name);
    QFile f(p);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(body);
    return p;
}

QByteArray slurp(const QString &p) {
    QFile f(p);
    EXPECT_TRUE(f.open(QIODevice::ReadOnly));
    return f.readAll();
}

}  // namespace

// ---- INV-5: template emits the documented shape -----------------

TEST(RoadmapFoldIn, Inv5TemplateShape) {
    Finding f;
    f.checkId = "clazy-no-trivial-copyable-in-list";
    f.file = "src/foo.cpp";
    f.line = 42;
    f.message = "Some bug message that should become the theme.";
    QList<Finding> findings = {f};
    QList<int> ids = {1500};
    const QString out = AuditEngine::templateRoadmapFoldInBlock(
        findings, ids, "2026-05-13");
    EXPECT_TRUE(out.startsWith("### 🔍 Audit fold-in (2026-05-13)"));
    EXPECT_TRUE(out.contains("- 📋 [ANTS-1500]"));
    EXPECT_TRUE(out.contains("Kind: audit-fix."));
    EXPECT_TRUE(out.contains("Source: audit-2026-05-13."));
    EXPECT_TRUE(out.contains("Lanes: foo."));
    EXPECT_TRUE(out.contains("`src/foo.cpp:42`"));
}

TEST(RoadmapFoldIn, TemplateEmptyOnSizeMismatch) {
    Finding f;
    QList<Finding> findings = {f, f};
    QList<int> ids = {1};  // wrong size
    EXPECT_TRUE(AuditEngine::templateRoadmapFoldInBlock(
        findings, ids, "2026-05-13").isEmpty());
    EXPECT_TRUE(AuditEngine::templateRoadmapFoldInBlock(
        {}, {}, "2026-05-13").isEmpty());
}

// ---- INV-6: allocateIds returns N consecutive ints --------------

TEST(RoadmapFoldIn, Inv6AllocateConsecutive) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), ".roadmap-counter", "100\n");

    auto a = RoadmapFoldIn::allocateIds(tmp.path(), 3);
    ASSERT_EQ(a.size(), 3);
    EXPECT_EQ(a[0], 101);
    EXPECT_EQ(a[1], 102);
    EXPECT_EQ(a[2], 103);

    // Counter file post-write contains the new value.
    EXPECT_EQ(slurp(QDir(tmp.path()).filePath(".roadmap-counter")).trimmed(),
              QByteArray("103"));

    // Second call continues from new value.
    auto b = RoadmapFoldIn::allocateIds(tmp.path(), 2);
    ASSERT_EQ(b.size(), 2);
    EXPECT_EQ(b[0], 104);
    EXPECT_EQ(b[1], 105);
}

TEST(RoadmapFoldIn, AllocateZeroOrNegativeReturnsEmpty) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), ".roadmap-counter", "1\n");
    EXPECT_TRUE(RoadmapFoldIn::allocateIds(tmp.path(), 0).isEmpty());
    EXPECT_TRUE(RoadmapFoldIn::allocateIds(tmp.path(), -1).isEmpty());
}

TEST(RoadmapFoldIn, AllocateMissingCounterFails) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // No .roadmap-counter present — open() in lockExclusive creates one
    // empty, then read returns "" → toInt fails → empty list.
    EXPECT_TRUE(RoadmapFoldIn::allocateIds(tmp.path(), 1).isEmpty());
}

// ---- INV-8: insertBlock places block after named heading --------

TEST(RoadmapFoldIn, Inv8InsertAfterHeading) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QByteArray rm =
        "# Test ROADMAP\n"
        "\n"
        "## 0.7.88 — audit fold (target: 2026-05)\n"
        "\n"
        "### Existing subsection\n"
        "- existing bullet\n"
        "\n"
        "## 0.7.87 — shipped\n";
    writeFile(tmp.path(), "ROADMAP.md", rm);

    const QString block =
        "### 🔍 Audit fold-in (2026-05-13)\n"
        "\n"
        "- 📋 [ANTS-9999] **Test bullet.**\n";
    EXPECT_TRUE(RoadmapFoldIn::insertBlock(
        tmp.path(),
        QStringLiteral("## 0.7.88 — audit fold (target: 2026-05)"),
        block));

    const QByteArray after = slurp(QDir(tmp.path()).filePath("ROADMAP.md"));
    // Block must appear AFTER the target heading and BEFORE the next
    // section.
    const int hPos = after.indexOf("## 0.7.88 — audit fold");
    const int bPos = after.indexOf("### 🔍 Audit fold-in (2026-05-13)");
    const int nextPos = after.indexOf("## 0.7.87 — shipped");
    ASSERT_GT(hPos, -1);
    ASSERT_GT(bPos, hPos);
    ASSERT_GT(nextPos, bPos);
}

TEST(RoadmapFoldIn, Inv8InsertHeadingNotFoundReturnsFalse) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QByteArray rm = "# Test\n## existing\n";
    writeFile(tmp.path(), "ROADMAP.md", rm);

    const QByteArray before = slurp(QDir(tmp.path()).filePath("ROADMAP.md"));
    EXPECT_FALSE(RoadmapFoldIn::insertBlock(
        tmp.path(),
        QStringLiteral("## non-existent heading"),
        QStringLiteral("### foo\n")));
    const QByteArray after = slurp(QDir(tmp.path()).filePath("ROADMAP.md"));
    EXPECT_EQ(before, after);  // unchanged
}

TEST(RoadmapFoldIn, InsertBlockTrimsTrailingNewlineFromHeading) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QByteArray rm = "## 0.7.88 — t (target: 2026-05)\n";
    writeFile(tmp.path(), "ROADMAP.md", rm);

    EXPECT_TRUE(RoadmapFoldIn::insertBlock(
        tmp.path(),
        QStringLiteral("## 0.7.88 — t (target: 2026-05)\n"),  // trailing \n
        QStringLiteral("### foo\n")));
}

// ---- INV-8b: findActiveReleaseHeading prefers in-flight ---------

TEST(RoadmapFoldIn, Inv8bFindPrefersInflight) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray rm =
        "# Roadmap\n"
        "\n"
        "## Distribution-adoption overview\n"
        "\n"
        "## 0.7.88 — audit fold (target: 2026-05)\n"
        "\n"
        "## 0.7.87 — MCP pack — shipped 2026-05-13\n";
    writeFile(tmp.path(), "ROADMAP.md", rm);

    const QString h = RoadmapFoldIn::findActiveReleaseHeading(tmp.path());
    EXPECT_EQ(h, QStringLiteral("## 0.7.88 — audit fold (target: 2026-05)"));
}

TEST(RoadmapFoldIn, Inv8bFindFallsBackToShipped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray rm =
        "# Roadmap\n"
        "\n"
        "## Some prose section\n"
        "\n"
        "## 0.7.87 — MCP pack — shipped 2026-05-13\n"
        "\n"
        "## 0.7.86 — older — shipped 2026-05-10\n";
    writeFile(tmp.path(), "ROADMAP.md", rm);

    const QString h = RoadmapFoldIn::findActiveReleaseHeading(tmp.path());
    EXPECT_EQ(h, QStringLiteral("## 0.7.87 — MCP pack — shipped 2026-05-13"));
}

TEST(RoadmapFoldIn, Inv8bFindEmptyForNoMatches) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray rm =
        "# Roadmap\n"
        "\n"
        "## Distribution-adoption overview\n"
        "\n"
        "## Per-store publication playbook\n";
    writeFile(tmp.path(), "ROADMAP.md", rm);

    EXPECT_TRUE(RoadmapFoldIn::findActiveReleaseHeading(tmp.path()).isEmpty());
}

TEST(RoadmapFoldIn, FindActiveReleaseHeadingMissingFile) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    EXPECT_TRUE(RoadmapFoldIn::findActiveReleaseHeading(tmp.path()).isEmpty());
}
