// ANTS-4817 — feature-conformance test for corroboration near misses.
//
// The measured gap: two lanes finding ONE defect and quoting it a single line
// apart read as no agreement, because corroboration keys on exact
// (file, line). Two readers quoting one statement rarely pick the same line —
// a multi-line call, a decorator or a docstring puts the quotable line
// somewhere different for each — so exact matching makes the highest-value
// agreements the least likely to be reported. Two independent reports measured
// the same rate: every real agreement in their runs was missed, and the
// envelope gave no sign that a near miss had occurred.
//
// Near misses are ADVISORY. They are not findings and do not change what
// corroboration means; they make a zero explainable. That boundary is what
// INV-2 and INV-4 pin.

#include "indiereviewengine.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QIODevice>
#include <QString>
#include <QTemporaryDir>

namespace {

// A project containing the one file every report below cites.
QString makeProject(QTemporaryDir &tmp) {
    const QString root = tmp.path();
    EXPECT_TRUE(QDir(root).mkpath(QStringLiteral("src")));
    QFile f(root + QStringLiteral("/src/window.py"));
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(4096, 'X'));
    f.close();
    return root;
}

}  // namespace

// INV-1 — two lanes citing one defect a line apart produce NO finding (exact
// matching is unchanged) but DO produce a near miss naming the span and both
// lanes. This is the finbreak / OneUp shape.
TEST(IndieReviewNearMiss, Inv1AdjacentCitationsFromTwoLanesAreANearMiss) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = makeProject(tmp);

    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("lane_a"),
        QStringLiteral("Unescaped path reaches the statement at "
                       "src/window.py:738."));
    reports.insert(QStringLiteral("lane_b"),
        QStringLiteral("The call on src/window.py:739 interpolates a raw "
                       "path."));

    IndieReviewEngine::CorroborateStats stats;
    const auto found =
        IndieReviewEngine::corroboratedFindings(root, reports, 2, &stats);

    EXPECT_TRUE(found.isEmpty())
        << "exact (file, line) corroboration must be unchanged — a near miss "
           "is not promoted to a finding";
    ASSERT_EQ(stats.nearMisses.size(), 1)
        << "two lanes citing one defect a line apart must surface as a near "
           "miss, or a zero is unexplainable";
    const auto &nm = stats.nearMisses.first();
    EXPECT_EQ(nm.file, QStringLiteral("src/window.py"));
    EXPECT_EQ(nm.lineFrom, 738);
    EXPECT_EQ(nm.lineTo, 739);
    EXPECT_EQ(nm.citingLanes.size(), 2);
    EXPECT_TRUE(nm.citingLanes.contains(QStringLiteral("lane_a")));
    EXPECT_TRUE(nm.citingLanes.contains(QStringLiteral("lane_b")));
}

// INV-2 — a real agreement stays a finding and is NOT also reported as a near
// miss. Double-counting would inflate the run's strongest signal.
TEST(IndieReviewNearMiss, Inv2ExactAgreementIsAFindingAndNotANearMiss) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = makeProject(tmp);

    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("lane_a"),
        QStringLiteral("Defect at src/window.py:738."));
    reports.insert(QStringLiteral("lane_b"),
        QStringLiteral("Also src/window.py:738."));

    IndieReviewEngine::CorroborateStats stats;
    const auto found =
        IndieReviewEngine::corroboratedFindings(root, reports, 2, &stats);

    ASSERT_EQ(found.size(), 1);
    EXPECT_EQ(found.first().line, 738);
    EXPECT_TRUE(stats.nearMisses.isEmpty())
        << "a (file, line) that already met min_lanes must not be re-reported "
           "as a near miss";
}

// INV-3 — citations far apart in one file are not agreement. The tolerance is
// narrow on purpose; widening it far enough to sweep in unrelated citations
// from a dense file is what would make this signal worthless.
TEST(IndieReviewNearMiss, Inv3DistantCitationsAreNotANearMiss) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = makeProject(tmp);

    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("lane_a"),
        QStringLiteral("Defect at src/window.py:100."));
    reports.insert(QStringLiteral("lane_b"),
        QStringLiteral("Different defect at src/window.py:400."));

    IndieReviewEngine::CorroborateStats stats;
    const auto found =
        IndieReviewEngine::corroboratedFindings(root, reports, 2, &stats);

    EXPECT_TRUE(found.isEmpty());
    EXPECT_TRUE(stats.nearMisses.isEmpty())
        << "two lanes citing unrelated places in one file are not a near miss";
}

// INV-4 — a near miss needs min_lanes DISTINCT lanes, exactly as a finding
// does. One lane quoting two adjacent lines of its own is one lane's opinion,
// not corroboration, and reporting it would be the fuzzy matching both
// reporting projects explicitly asked not to have.
TEST(IndieReviewNearMiss, Inv4OneLaneCitingAdjacentLinesIsNotANearMiss) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = makeProject(tmp);

    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("lane_a"),
        QStringLiteral("Defect spans src/window.py:738 and "
                       "src/window.py:739."));
    reports.insert(QStringLiteral("lane_b"),
        QStringLiteral("Nothing to report."));

    IndieReviewEngine::CorroborateStats stats;
    const auto found =
        IndieReviewEngine::corroboratedFindings(root, reports, 2, &stats);

    EXPECT_TRUE(found.isEmpty());
    EXPECT_TRUE(stats.nearMisses.isEmpty())
        << "a single lane's own adjacent citations are not agreement";
}

// INV-5 — the opt-in `lineSlop` promotes a near miss to a finding naming the
// SPAN. This is the half that changes what corroboration reports, which is
// exactly why it is opt-in.
TEST(IndieReviewNearMiss, Inv5LineSlopPromotesToASpanFinding) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = makeProject(tmp);

    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("lane_a"),
        QStringLiteral("Defect at src/window.py:738."));
    reports.insert(QStringLiteral("lane_b"),
        QStringLiteral("Same defect, quoted at src/window.py:739."));

    IndieReviewEngine::CorroborateStats stats;
    const auto found = IndieReviewEngine::corroboratedFindings(
        root, reports, 2, &stats, /*lineSlop=*/3);

    ASSERT_EQ(found.size(), 1)
        << "line_slop must group the two citations into one finding";
    EXPECT_EQ(found.first().file, QStringLiteral("src/window.py"));
    EXPECT_EQ(found.first().line, 738);
    EXPECT_EQ(found.first().lineTo, 739)
        << "a promoted finding names the SPAN — the lanes disagree about the "
           "line, which is the whole reason it was not an exact match";
    EXPECT_EQ(found.first().citingLanes.size(), 2);
    EXPECT_TRUE(stats.nearMisses.isEmpty())
        << "a group promoted to a finding must not ALSO be reported as a near "
           "miss — that would double-count it";
}

// INV-6 — the default is 0, and at 0 nothing is promoted. This is what keeps
// every existing report meaning what it meant; both reporting projects asked
// for exactly this.
TEST(IndieReviewNearMiss, Inv6DefaultSlopIsZeroAndPromotesNothing) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = makeProject(tmp);

    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("lane_a"),
        QStringLiteral("Defect at src/window.py:738."));
    reports.insert(QStringLiteral("lane_b"),
        QStringLiteral("Same defect, quoted at src/window.py:739."));

    // Default (argument omitted) and an explicit 0 must agree.
    IndieReviewEngine::CorroborateStats sDefault;
    const auto byDefault =
        IndieReviewEngine::corroboratedFindings(root, reports, 2, &sDefault);
    IndieReviewEngine::CorroborateStats sZero;
    const auto byZero = IndieReviewEngine::corroboratedFindings(
        root, reports, 2, &sZero, /*lineSlop=*/0);

    EXPECT_TRUE(byDefault.isEmpty())
        << "the default must not group — that would redefine corroboration "
           "for every caller who never asked";
    EXPECT_EQ(byDefault.size(), byZero.size());
    EXPECT_EQ(sDefault.nearMisses.size(), sZero.nearMisses.size());
    EXPECT_EQ(sDefault.nearMisses.size(), 1)
        << "the group is still reported, as an advisory near miss";
}
