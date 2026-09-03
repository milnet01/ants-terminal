// ANTS-4814 — feature-conformance test for shared-symbol corroboration.
//
// The measured gap: corroboration keys on (file, line), so several lanes
// finding one defect SHAPE at unrelated locations read as no agreement. On a
// partition BY SUBSYSTEM that is the agreement that matters — lanes do not
// share files, so identical file:line is the one form of agreement the
// partition makes unlikely. A reported run whose lanes agreed twice over came
// back with a single corroborated finding.
//
// The grouping is mechanical: the UNQUALIFIED enclosing symbol, resolved from
// the file outline. Unqualified because the measured case was one defect
// repeated across sibling classes, whose qualified names differ by
// construction. No similarity scoring — both reporting projects explicitly
// asked not to have fuzzy matching, and that restraint is the useful part.
//
// Behavioural: drives RemoteControl::cmdIndieReviewCorroborateForTest, because
// the grouping lives at the MCP layer (the engine is Qt6::Core-only and has no
// file-outline dependency).

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

ANTS_TEST_SCOPE();

namespace {

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

// Three sibling views, each with the SAME method name in a DIFFERENT class —
// the Games_Hub shape. The defect line in each is inside scheduleEngineMove.
QByteArray viewSource(const QByteArray &cls) {
    return QByteArray(
        "#include \"x.h\"\n"
        "\n"
        "void ") + cls + "::paint() {\n"
        "    // padding\n"
        "    // padding\n"
        "}\n"
        "\n"
        "void " + cls + "::scheduleEngineMove() {\n"
        "    // the unguarded move lands here\n"
        "    engine->move();\n"
        "}\n";
}

QString makeProject(QTemporaryDir &tmp) {
    const QString root = tmp.path();
    EXPECT_TRUE(QDir(root).mkpath(QStringLiteral("src")));
    EXPECT_TRUE(writeFile(root + QStringLiteral("/src/chessview.cpp"),
                          viewSource("ChessView")));
    EXPECT_TRUE(writeFile(root + QStringLiteral("/src/reversiview.cpp"),
                          viewSource("ReversiView")));
    EXPECT_TRUE(writeFile(root + QStringLiteral("/src/draughtsview.cpp"),
                          viewSource("DraughtsView")));
    return root;
}

QJsonObject corroborate(const QString &root, const QJsonObject &reports) {
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    r[QStringLiteral("reports")]    = reports;
    return RemoteControl(nullptr).cmdIndieReviewCorroborateForTest(r).object();
}

QJsonObject sharedNamed(const QJsonObject &env, const QString &symbol) {
    for (const auto v : env.value(QStringLiteral("shared_symbols")).toArray()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("symbol")).toString() == symbol) return o;
    }
    return {};
}

}  // namespace

// INV-1 — three lanes citing one defect SHAPE in three different files, inside
// the same-named method of three sibling classes, surface as one shared
// symbol. Exact matching still reports no findings.
TEST(IndieReviewSharedSymbol, Inv1SameShapeAcrossFilesGroupsBySymbol) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = makeProject(tmp);

    QJsonObject reports;
    reports[QStringLiteral("chess")] = QStringLiteral(
        "Unguarded scheduled move at src/chessview.cpp:10.");
    reports[QStringLiteral("reversi")] = QStringLiteral(
        "Same unguarded move at src/reversiview.cpp:10.");
    reports[QStringLiteral("draughts")] = QStringLiteral(
        "And again at src/draughtsview.cpp:10.");

    const QJsonObject env = corroborate(root, reports);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(env).toJson().toStdString();

    EXPECT_EQ(env.value(QStringLiteral("total_findings")).toInt(), 0)
        << "exact (file, line) corroboration must be unchanged";

    const QJsonObject g =
        sharedNamed(env, QStringLiteral("scheduleEngineMove"));
    ASSERT_FALSE(g.isEmpty())
        << "three lanes citing one defect shape in three files must surface "
           "as a shared symbol: " << QJsonDocument(env).toJson().toStdString();
    EXPECT_EQ(g.value(QStringLiteral("citing_lanes")).toArray().size(), 3);
    EXPECT_EQ(g.value(QStringLiteral("file_count")).toInt(), 3);
}

// INV-2 — a symbol cited by only ONE lane is not agreement, however many
// places that lane cites it. min_lanes applies here exactly as it does to a
// finding.
TEST(IndieReviewSharedSymbol, Inv2OneLaneAcrossFilesIsNotAgreement) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = makeProject(tmp);

    QJsonObject reports;
    reports[QStringLiteral("chess")] = QStringLiteral(
        "Unguarded at src/chessview.cpp:10 and src/reversiview.cpp:10.");
    reports[QStringLiteral("other")] = QStringLiteral("Nothing to report.");

    const QJsonObject env = corroborate(root, reports);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(sharedNamed(env, QStringLiteral("scheduleEngineMove")).isEmpty())
        << "one lane citing a symbol in several files is one lane's opinion";
}

// INV-3 — two lanes agreeing inside ONE file are findings-or-near-misses
// territory, not this signal. Requiring two distinct files is what keeps the
// three signals disjoint, so one agreement is never reported three times.
TEST(IndieReviewSharedSymbol, Inv3SingleFileAgreementIsNotASharedSymbol) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = makeProject(tmp);

    QJsonObject reports;
    reports[QStringLiteral("lane_a")] = QStringLiteral(
        "Defect at src/chessview.cpp:10.");
    reports[QStringLiteral("lane_b")] = QStringLiteral(
        "Confirmed at src/chessview.cpp:10.");

    const QJsonObject env = corroborate(root, reports);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("total_findings")).toInt(), 1)
        << "an exact agreement is still a finding";
    EXPECT_TRUE(sharedNamed(env, QStringLiteral("scheduleEngineMove")).isEmpty())
        << "an agreement already reported as a finding must not be repeated "
           "as a shared symbol";
}

// INV-4 — the field is absent entirely when there is nothing to report, so an
// ordinary run's envelope is unchanged.
TEST(IndieReviewSharedSymbol, Inv4AbsentWhenNothingIsShared) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = makeProject(tmp);

    QJsonObject reports;
    reports[QStringLiteral("lane_a")] = QStringLiteral("Nothing found.");
    reports[QStringLiteral("lane_b")] = QStringLiteral("Nothing here either.");

    const QJsonObject env = corroborate(root, reports);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(env.contains(QStringLiteral("shared_symbols")))
        << "the envelope must stay byte-identical when nothing is shared";
    EXPECT_FALSE(env.contains(QStringLiteral("shared_symbols_count")));
}
