// Feature-conformance test for tests/features/indie_review_brief_manifest/spec.md.
//
// ANTS-1281: assembleBriefManifest returns a brief manifest without
// inlined source bodies; the dispatched subagent reads files itself.
//
// Invariants exercised:
//   INV-2          — sourcePaths reflects lane.sourcePaths (canon-filtered)
//   INV-4          — path-traversal entries dropped from both list and brief
//   INV-5          — read-instruction sentinel present in brief
//   INV-6          — contractDocs == fixed 3-element authored-order list
//   INV-token-cost — 100 KiB synthetic source → brief < 8 KiB
//   INV-no-file-marker — brief does not contain "=== file: "

#include "indiereviewengine.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

// Materialise a fake project tree with one src/<name>.cpp file of
// the requested body length. Returns the project's absolute path.
QString makeFakeProject(QTemporaryDir &tmp,
                        const QString &laneName,
                        const QByteArray &body) {
    const QString root = tmp.path();
    QDir(root).mkpath(QStringLiteral("src"));
    const QString srcPath = root + QStringLiteral("/src/") + laneName
                            + QStringLiteral(".cpp");
    QFile f(srcPath);
    if (!f.open(QIODevice::WriteOnly)) return {};
    f.write(body);
    f.close();
    return root;
}

}  // namespace

TEST(IndieReviewBriefManifest, NoFileMarkersAndSentinel) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QByteArray body(2048, 'X');  // 2 KiB filler
    const QString root = makeFakeProject(tmp, QStringLiteral("foo"), body);
    ASSERT_FALSE(root.isEmpty());

    IndieReviewEngine::Lane lane;
    lane.name        = QStringLiteral("foo");
    lane.summary     = QStringLiteral("synthetic lane for ANTS-1281 test");
    lane.sourcePaths = {QStringLiteral("src/foo.cpp")};

    const auto m = IndieReviewEngine::assembleBriefManifest(root, lane);

    // INV-no-file-marker: no v1-style body sections.
    EXPECT_EQ(m.brief.indexOf(QStringLiteral("=== file: ")), -1)
        << "manifest.brief unexpectedly contains the v1 body-marker; "
           "this proves source bodies are still being inlined.";

    // INV-5: read-instruction sentinel.
    EXPECT_NE(m.brief.indexOf(QStringLiteral(
        "Read each source file in the list above using your Read tool")),
        -1) << "manifest.brief is missing the read-instruction sentinel; "
               "subagents won't know source bodies are intentionally absent.";
}

TEST(IndieReviewBriefManifest, SourcePathsAndContractDocs) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString root = makeFakeProject(
        tmp, QStringLiteral("bar"), QByteArray(64, 'Y'));
    ASSERT_FALSE(root.isEmpty());

    IndieReviewEngine::Lane lane;
    lane.name        = QStringLiteral("bar");
    lane.sourcePaths = {QStringLiteral("src/bar.cpp")};

    const auto m = IndieReviewEngine::assembleBriefManifest(root, lane);

    // INV-2: sourcePaths reflects lane.sourcePaths.
    ASSERT_EQ(m.sourcePaths.size(), 1);
    EXPECT_EQ(m.sourcePaths.first(), QStringLiteral("src/bar.cpp"));

    // INV-6: contract docs are the fixed 3-element authored-order list.
    const QStringList expected = {
        QStringLiteral("docs/standards/coding.md"),
        QStringLiteral("docs/standards/testing.md"),
        QStringLiteral("docs/standards/documentation.md"),
    };
    EXPECT_EQ(m.contractDocs, expected);

    // External specs / dimension weighting are reserved-empty in v1.
    EXPECT_TRUE(m.externalSpecs.isEmpty());
}

TEST(IndieReviewBriefManifest, PathTraversalDroppedFromBothListAndBrief) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString root = makeFakeProject(
        tmp, QStringLiteral("baz"), QByteArray(64, 'Z'));
    ASSERT_FALSE(root.isEmpty());

    IndieReviewEngine::Lane lane;
    lane.name = QStringLiteral("baz");
    // Legit + malicious entries side-by-side.
    lane.sourcePaths = {
        QStringLiteral("src/baz.cpp"),
        QStringLiteral("../../etc/passwd"),
    };

    const auto m = IndieReviewEngine::assembleBriefManifest(root, lane);

    // INV-4: traversal entry dropped from sourcePaths.
    ASSERT_EQ(m.sourcePaths.size(), 1);
    EXPECT_EQ(m.sourcePaths.first(), QStringLiteral("src/baz.cpp"));

    // INV-4: traversal entry also absent from the brief's listed paths.
    EXPECT_EQ(m.brief.indexOf(QStringLiteral("../../etc/passwd")), -1)
        << "Traversal entry leaked into brief's source-path list; "
           "v2 tightening regressed to v1 behaviour.";
}

TEST(IndieReviewBriefManifest, TokenCostRegression100KiBToUnder8KiB) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    // 100 KiB synthetic source file.
    const QByteArray big(100 * 1024, 'A');
    const QString root = makeFakeProject(
        tmp, QStringLiteral("heavy"), big);
    ASSERT_FALSE(root.isEmpty());

    IndieReviewEngine::Lane lane;
    lane.name        = QStringLiteral("heavy");
    lane.summary     = QStringLiteral("synthetic 100 KiB lane");
    lane.sourcePaths = {QStringLiteral("src/heavy.cpp")};

    const auto m = IndieReviewEngine::assembleBriefManifest(root, lane);

    // INV-token-cost: brief stays under 8 KiB regardless of source size.
    const int briefBytes = m.brief.toUtf8().size();
    EXPECT_LT(briefBytes, 8 * 1024)
        << "brief grew to " << briefBytes
        << " bytes — body-stripping is broken; v1 would emit ~100 KiB here.";
}
