// Feature-conformance test for
// tests/features/review_corroboration_single_pass/spec.md.
//
// Why this exists: ANTS-5125 — IndieReviewDialog::onAllReportsCollected and
// ColdEyesDialog::onAllReportsCollected each run the corroboration walk
// twice per round (minLanes=2, then minLanes=1, diffed by key) instead of
// once at minLanes=1 split by IndieReviewEngine::splitByLaneCount. INV-1
// pins the split seam the fix is built on (holds today); INV-2 pins the
// call count in each dialog (does not hold today).

#include "../../_support/srcgrep.h"

#include "indiereviewengine.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <string>

namespace {

// Three lanes: laneA and laneB both cite src/vtparser.cpp:42 (agree — a
// minLanes=2 finding); laneC alone cites src/other.cpp:7 (a single-lane
// finding, present only at minLanes=1).
QString makeFixtureProject(QTemporaryDir &tmp, QHash<QString, QString> &reports) {
    const QString root = tmp.path();
    QDir(root).mkpath(QStringLiteral("src"));

    QFile vt(root + QStringLiteral("/src/vtparser.cpp"));
    EXPECT_TRUE(vt.open(QIODevice::WriteOnly));
    vt.write(QByteArray(64, 'X'));
    vt.close();

    QFile other(root + QStringLiteral("/src/other.cpp"));
    EXPECT_TRUE(other.open(QIODevice::WriteOnly));
    other.write(QByteArray(64, 'Y'));
    other.close();

    reports.insert(QStringLiteral("laneA"),
        QStringLiteral("laneA flags src/vtparser.cpp:42 as suspicious.\n"));
    reports.insert(QStringLiteral("laneB"),
        QStringLiteral("laneB independently flags src/vtparser.cpp:42.\n"));
    reports.insert(QStringLiteral("laneC"),
        QStringLiteral("laneC alone flags src/other.cpp:7.\n"));
    return root;
}

// Compare the observable fields the spec pins — file, line, citingLanes —
// ignoring contexts/lineTo, which INV-1 makes no claim about.
::testing::AssertionResult SameObservableFindings(
    const char *, const char *,
    const QList<IndieReviewEngine::CorroboratedFinding> &a,
    const QList<IndieReviewEngine::CorroboratedFinding> &b) {
    if (a.size() != b.size()) {
        return ::testing::AssertionFailure()
            << "size mismatch: " << a.size() << " vs " << b.size();
    }
    for (int i = 0; i < a.size(); ++i) {
        if (a[i].file != b[i].file || a[i].line != b[i].line ||
            a[i].citingLanes != b[i].citingLanes) {
            return ::testing::AssertionFailure()
                << "mismatch at index " << i << ": "
                << "(" << a[i].file.toStdString() << ":" << a[i].line
                << " lanes=" << a[i].citingLanes.join(',').toStdString()
                << ") vs ("
                << b[i].file.toStdString() << ":" << b[i].line
                << " lanes=" << b[i].citingLanes.join(',').toStdString()
                << ")";
        }
    }
    return ::testing::AssertionSuccess();
}

}  // namespace

// INV-1 — splitByLaneCount(corroboratedFindings(root, reports, 1)) equals,
// field for field, the pair the dialogs build today from two separate
// walks: a minLanes=2 call for `.corroborated`, and the minLanes=1
// findings not present in that minLanes=2 result (by (file, line)) for
// `.singleLane`.
TEST(ReviewCorroborationSinglePass, Inv1SplitMatchesTwoSeparateCalls) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QHash<QString, QString> reports;
    const QString root = makeFixtureProject(tmp, reports);

    const auto min1 = IndieReviewEngine::corroboratedFindings(root, reports, 1);
    const auto min2 = IndieReviewEngine::corroboratedFindings(root, reports, 2);
    ASSERT_EQ(min1.size(), 2)
        << "precondition: fixture should yield one 2-lane and one 1-lane "
           "finding at minLanes=1";
    ASSERT_EQ(min2.size(), 1)
        << "precondition: fixture should yield exactly the 2-lane finding "
           "at minLanes=2";

    // The old two-call shape, reproduced by hand as the reference.
    QSet<QString> min2Keys;
    for (const auto &f : min2)
        min2Keys.insert(f.file + QChar(':') + QString::number(f.line));
    QList<IndieReviewEngine::CorroboratedFinding> expectedSingleLane;
    for (const auto &f : min1) {
        const QString k = f.file + QChar(':') + QString::number(f.line);
        if (!min2Keys.contains(k)) expectedSingleLane << f;
    }
    ASSERT_EQ(expectedSingleLane.size(), 1)
        << "precondition: exactly one finding should be single-lane-only";

    const auto split = IndieReviewEngine::splitByLaneCount(min1, 2);

    EXPECT_PRED_FORMAT2(SameObservableFindings, split.corroborated, min2);
    EXPECT_PRED_FORMAT2(SameObservableFindings, split.singleLane,
                         expectedSingleLane);

    // Sanity on the content, not just the shape: the 2-lane finding is
    // vtparser.cpp:42, the single-lane one is other.cpp:7.
    ASSERT_EQ(split.corroborated.size(), 1);
    EXPECT_EQ(split.corroborated.first().file, QStringLiteral("src/vtparser.cpp"));
    EXPECT_EQ(split.corroborated.first().line, 42);
    EXPECT_EQ(split.corroborated.first().citingLanes.size(), 2);
    ASSERT_EQ(split.singleLane.size(), 1);
    EXPECT_EQ(split.singleLane.first().file, QStringLiteral("src/other.cpp"));
    EXPECT_EQ(split.singleLane.first().line, 7);
    EXPECT_EQ(split.singleLane.first().citingLanes.size(), 1);
}

// INV-2 — each dialog's onAllReportsCollected body calls its corroboration
// entry point exactly once and uses splitByLaneCount to get both halves.
// Source scrape: a behavioural test can't observe how many times a dialog
// walked the tree internally, so a call-count grep is the only way to hold
// "once per round". Anchored on the qualified signature, not the bare
// method name, which can match a connect() reference first.
TEST(ReviewCorroborationSinglePass, Inv2IndieReviewDialogCallsOnce) {
    const std::string src =
        ants_test::slurpFile(SRC_INDIEREVIEWDIALOG_CPP_PATH);
    ASSERT_FALSE(src.empty())
        << "precondition: could not read indiereviewdialog.cpp at "
           "SRC_INDIEREVIEWDIALOG_CPP_PATH";
    const std::string stripped = ants_test::stripComments(src);

    const std::string body = ants_test::slurpFunctionBody(
        stripped, "void IndieReviewDialog::onAllReportsCollected(");
    ASSERT_FALSE(body.empty())
        << "precondition: onAllReportsCollected body not located in "
           "indiereviewdialog.cpp — update this scrape's anchor if the "
           "function was renamed or restructured.";

    const std::size_t calls =
        ants_test::countOccurrences(body, "corroboratedFindings(");
    EXPECT_EQ(calls, 1u)
        << "INV-2: IndieReviewDialog::onAllReportsCollected calls "
           "IndieReviewEngine::corroboratedFindings( " << calls
           << " times in its (comment-stripped) body; expected exactly 1. "
           "A second call re-runs the whole basename-index tree walk on "
           "the GUI thread for a result splitByLaneCount can derive from "
           "the first.";
    EXPECT_NE(body.find("splitByLaneCount("), std::string::npos)
        << "INV-2: IndieReviewDialog::onAllReportsCollected never calls "
           "splitByLaneCount( — the corroborated/single-lane halves must "
           "come from splitting one corroboratedFindings(..., 1) result, "
           "not from a second walk.";
}

TEST(ReviewCorroborationSinglePass, Inv2ColdEyesDialogCallsOnce) {
    const std::string src =
        ants_test::slurpFile(SRC_COLDEYESDIALOG_CPP_PATH);
    ASSERT_FALSE(src.empty())
        << "precondition: could not read coldeyesdialog.cpp at "
           "SRC_COLDEYESDIALOG_CPP_PATH";
    const std::string stripped = ants_test::stripComments(src);

    const std::string body = ants_test::slurpFunctionBody(
        stripped, "void ColdEyesDialog::onAllReportsCollected(");
    ASSERT_FALSE(body.empty())
        << "precondition: onAllReportsCollected body not located in "
           "coldeyesdialog.cpp — update this scrape's anchor if the "
           "function was renamed or restructured.";

    const std::size_t calls =
        ants_test::countOccurrences(body, "crossDocDiffFromReports(");
    EXPECT_EQ(calls, 1u)
        << "INV-2: ColdEyesDialog::onAllReportsCollected calls "
           "ColdEyesEngine::crossDocDiffFromReports( " << calls
           << " times in its (comment-stripped) body; expected exactly 1. "
           "crossDocDiffFromReports delegates straight to "
           "IndieReviewEngine::corroboratedFindings, so a second call pays "
           "for the same tree walk twice on the GUI thread.";
    EXPECT_NE(body.find("splitByLaneCount("), std::string::npos)
        << "INV-2: ColdEyesDialog::onAllReportsCollected never calls "
           "splitByLaneCount( — the corroborated/single-lane halves must "
           "come from splitting one crossDocDiffFromReports(..., 1) "
           "result, not from a second walk.";
}
