// Review Changes probe bounds and single finalize — see spec.md. ANTS-5109.

#include <QFile>
#include <QFileInfo>
#include <QString>

#include <gtest/gtest.h>

namespace {

QString source() {
    QFile f(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
            + QStringLiteral("/../../../src/diffviewer.cpp"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

}  // namespace

// INV-1
TEST(ReviewChangesProbeGuards, CrossBranchLogIsBounded) {
    const QString s = source();
    const int log = s.indexOf(QStringLiteral("runAsync({\"log\", \"--branches\", \"--not\", \"--remotes\","));
    ASSERT_GE(log, 0);
    const int end = s.indexOf(QStringLiteral("&ProbeState::crossUnpushed"), log);
    ASSERT_GT(end, log);
    EXPECT_TRUE(s.mid(log, end - log).contains(QStringLiteral("--max-count=")))
        << "with no remote refs the unpushed log lists the whole history";
}

// INV-2
TEST(ReviewChangesProbeGuards, ErrorHandlerFinalizesOnlyOnFailedStart) {
    const QString s = source();
    const int conn = s.indexOf(QStringLiteral("QObject::connect(p, &QProcess::errorOccurred, dialog,"));
    ASSERT_GE(conn, 0);
    const int fin = s.indexOf(QStringLiteral("finalize();"), conn);
    ASSERT_GT(fin, conn);
    EXPECT_TRUE(s.mid(conn, fin - conn).contains(QStringLiteral("QProcess::FailedToStart")))
        << "a crashed git runs finalize() from both errorOccurred and finished";
}
