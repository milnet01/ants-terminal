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
#include "../../_support/srcgrep.h"  // ANTS-3833 — slurpRemoteControl

#if !defined(ANTS_SOURCE_DIR) || !defined(SRC_MAINWINDOW_CPP_PATH) || \
    !defined(ANTS_RC_SOURCES) || !defined(SRC_CLAUDE_INTEGRATION_CPP_PATH)
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
        {Kind::DeadAnchor,      "docs/a.md", 1, "m1"},
        {Kind::BrokenLink,      "docs/a.md", 2, "m2"},
        {Kind::TocGap,          "docs/a.md", 3, "m3"},
        {Kind::HeadingSequence, "docs/a.md", 4, "m4"},   // ANTS-3700
        {Kind::UngrantedTool,   "docs/a.md", 5, "m5"},   // ANTS-3719
    };

    // Unfiltered → every kind present.
    const QJsonObject all = RemoteControl::docIntegrityBuildResponse(
        findings, {}, {"docs/a.md"});
    EXPECT_EQ(all.value("findings").toArray().size(), 5);
    const QJsonObject allCounts = all.value("counts").toObject();
    EXPECT_EQ(allCounts.value("dead_anchor").toInt(), 1);
    EXPECT_EQ(allCounts.value("broken_link").toInt(), 1);
    EXPECT_EQ(allCounts.value("toc_gap").toInt(), 1);
    // ANTS-3700 — the new kind counts as ITSELF. Before the counter became a
    // switch, an unrecognised kind fell through the `else` and inflated
    // toc_gap, so a heading_sequence finding would have been counted twice
    // over: once correctly, once as a TOC defect that did not exist.
    EXPECT_EQ(allCounts.value("heading_sequence").toInt(), 1);
    EXPECT_EQ(allCounts.value("ungranted_tool").toInt(), 1);      // ANTS-3719

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
    EXPECT_FALSE(onlyCounts.contains("heading_sequence"));
    EXPECT_FALSE(onlyCounts.contains("ungranted_tool"));

    // ANTS-3719 — the newest kind filters like any other, and its own count
    // survives the narrowing. Same shape as the ANTS-3700 case below, kept
    // explicit because the counter is a switch: a kind added to the enum and
    // not to the switch fails to compile, but one added to the switch and not
    // to the counts map narrows to nothing silently.
    const QJsonObject ug = RemoteControl::docIntegrityBuildResponse(
        findings, QSet<QString>{"ungranted_tool"}, {"docs/a.md"});
    const QJsonArray ugFs = ug.value("findings").toArray();
    ASSERT_EQ(ugFs.size(), 1);
    EXPECT_EQ(ugFs.at(0).toObject().value("kind").toString(),
              QStringLiteral("ungranted_tool"));
    EXPECT_EQ(ug.value("counts").toObject().value("ungranted_tool").toInt(), 1);

    // The new kind filters like any other (ANTS-3700).
    const QJsonObject seq = RemoteControl::docIntegrityBuildResponse(
        findings, QSet<QString>{"heading_sequence"}, {"docs/a.md"});
    const QJsonArray seqFs = seq.value("findings").toArray();
    ASSERT_EQ(seqFs.size(), 1);
    EXPECT_EQ(seqFs.at(0).toObject().value("kind").toString(),
              QStringLiteral("heading_sequence"));
    EXPECT_EQ(seq.value("counts").toObject().value("heading_sequence").toInt(), 1);
}

// INV-10 — verb contract wiring: caller_cwd Required, path validation →
// bad_path, ETag-304. Source-scrape the registration sites.
TEST(DocIntegrityVerb, WiringRegistered) {
    const QString mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.isEmpty());
    // Registered with the Required caller_cwd contract.
    const int reg = mw.indexOf("registerToolProvider(\"doc_integrity\"");
    ASSERT_GE(reg, 0);
    // ANTS-3681 — the registration entry, bounded by the next one.
    EXPECT_TRUE(QString::fromStdString(ants_test::regionBetween(
                    mw.toStdString(), "registerToolProvider(\"doc_integrity\"",
                    "registerToolProvider("))
                    .contains("CallerCwdContract::Required"));

    const QString ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.isEmpty());
    EXPECT_TRUE(ci.contains("\"doc_integrity\""));  // schema + registrations
    // callerCwdContractFor → Required for doc_integrity.
    const int cc = ci.indexOf("toolName == QStringLiteral(\"doc_integrity\")");
    ASSERT_GE(cc, 0);
    EXPECT_TRUE(ci.mid(cc, 100).contains("C::Required"));

    const QString rc = QString::fromStdString(ants_test::slurpRemoteControl());
    ASSERT_FALSE(rc.isEmpty());
    const int h = rc.indexOf("RemoteControl::cmdDocIntegrity");
    ASSERT_GE(h, 0);
    // The handler validates a supplied path and refuses bad_path on escape.
    // Window widened for ANTS-4106's `paths[]` block (validation loop +
    // rationale comment) — the enumerateMany dispatch is the last line of the
    // handler's scoping section.
    const QString body = rc.mid(h, 3600);
    EXPECT_TRUE(body.contains("validatePath("));
    EXPECT_TRUE(body.contains("check.err"));
    // INV-18 (ANTS-4106) — every `paths[]` entry is validated the same way, so
    // a root-escape in the plural form refuses bad_path exactly as `path` does.
    EXPECT_TRUE(body.contains("QStringLiteral(\"paths\")"));
    EXPECT_TRUE(body.contains("docIntegrityEnumerateMany"));
    // Declared on the schema, else a caller cannot reach it —
    // additionalProperties:false would refuse the argument outright.
    EXPECT_TRUE(ci.contains("props[\"paths\"]      = pathsProp;"));
}

// INV-18 (ANTS-4106) — `paths:[…]` checks the union of several files/dirs in
// one run. /apply-fixes step 5 says to pass the files a run edited, precisely
// to avoid a whole-tree walk, and a real pass edits a handful spread across
// the tree that no single `path` covers — so the whole-tree walk was what
// happened and a pre-existing finding in an untouched file got attributed to
// the pass.
TEST(DocIntegrityVerb, EnumerateManyUnion) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(writeFile(root + "/docs/a/one.md", "# H\n"));
    ASSERT_TRUE(writeFile(root + "/docs/a/sub/two.md", "# H\n"));
    ASSERT_TRUE(writeFile(root + "/docs/b/three.md", "# H\n"));
    ASSERT_TRUE(writeFile(root + "/README.md", "# H\n"));
    ASSERT_TRUE(writeFile(root + "/CHANGELOG.md", "# H\n"));

    // The shape a fix pass actually has: two root files + one docs subtree.
    const QStringList u = RemoteControl::docIntegrityEnumerateMany(
        root, {"README.md", "CHANGELOG.md", "docs/a"}, "docs");
    EXPECT_EQ(u, (QStringList{"CHANGELOG.md", "README.md",
                              "docs/a/one.md", "docs/a/sub/two.md"}))
        << "sorted union, and docs/b is NOT walked";

    // Overlapping entries collapse — a dir plus a file inside it is one doc,
    // not two, so no document can be counted into two findings.
    EXPECT_EQ(RemoteControl::docIntegrityEnumerateMany(
                  root, {"docs/a", "docs/a/one.md"}, "docs"),
              (QStringList{"docs/a/one.md", "docs/a/sub/two.md"}));

    // A non-existent in-root entry contributes nothing and does not refuse
    // (INV-15's rule, per entry).
    EXPECT_EQ(RemoteControl::docIntegrityEnumerateMany(
                  root, {"docs/b", "docs/nope"}, "docs"),
              (QStringList{"docs/b/three.md"}));

    // Empty list → empty, so the handler can tell "no paths given" from
    // "paths matched nothing" and fall back to the single-path walk.
    EXPECT_TRUE(RemoteControl::docIntegrityEnumerateMany(root, {}, "docs").isEmpty());
}

// INV-17 (ANTS-3737) — the checked-doc-set digest is content-sensitive, which
// is what makes the central ETag content-sensitive for the three findings-only
// doc verbs. Without it, editing docs that produce no new finding leaves the
// response envelope byte-identical, so `etag_match` answers a false 304 and a
// post-fix re-check is skipped — exactly when it is looking for a NEW finding
// the fix introduced. Fin Break feedback, 2026-07-30.
TEST(DocIntegrityVerb, DocSetDigestTracksContent) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(writeFile(root + "/docs/one.md", "# One\n\nAlpha prose.\n"));
    ASSERT_TRUE(writeFile(root + "/docs/two.md", "# Two\n\nBeta prose.\n"));

    const QStringList docs =
        RemoteControl::docIntegrityEnumerate(root, QString(), "docs");
    ASSERT_EQ(docs.size(), 2);

    const QString before = RemoteControl::docSetDigest(root, docs);
    EXPECT_FALSE(before.isEmpty());

    // Re-running with nothing touched must be stable — the 304 has to keep
    // working, or the fix would just disable the cache.
    EXPECT_EQ(RemoteControl::docSetDigest(root, docs), before);

    // A substantive edit that introduces NO finding must still bust it.
    ASSERT_TRUE(writeFile(root + "/docs/two.md",
                          "# Two\n\nBeta prose, materially rewritten.\n"));
    const QString after = RemoteControl::docSetDigest(root, docs);
    EXPECT_NE(after, before)
        << "a content edit that changes no finding must change the digest";

    // The digest covers the SET, not just the bytes: dropping a doc changes it.
    EXPECT_NE(RemoteControl::docSetDigest(root, {docs.first()}), after);
}
