// Roadmap dialog filters every kind and reads a large live roadmap — see
// spec.md. ANTS-5088.

#include "roadmapdialog.h"
#include "roadmapparse.h"

#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

// INV-1
TEST(RoadmapDialogKindsCap, EveryCanonicalKindHasAFilter) {
    QFile src(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
              + QStringLiteral("/../../../src/roadmapdialog.cpp"));
    ASSERT_TRUE(src.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromUtf8(src.readAll());
    for (const QString &kind : RoadmapParse::canonicalKinds()) {
        EXPECT_TRUE(text.contains(QStringLiteral("\"roadmap-filter-kind-") + kind + QLatin1Char('"')))
            << "no kind filter for " << kind.toStdString();
    }
    EXPECT_FALSE(text.contains(QStringLiteral("if (k == QStringLiteral(\"ux\")) return")))
        << "the card glyph lookup keeps its own kind list";
}

// INV-2
TEST(RoadmapDialogKindsCap, LiveRoadmapPastEightMiBIsRead) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("ROADMAP.md"));
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("# Roadmap\n\n");
        const QByteArray line(99, 'x');
        for (qint64 written = 0; written < 9 * 1024 * 1024; written += 100) {
            f.write(line);
            f.write("\n");
        }
        f.write("TAIL-MARKER-ANTS-5088\n");
    }
    const QString md = RoadmapDialog::loadMarkdown(path, false);
    EXPECT_TRUE(md.contains(QStringLiteral("TAIL-MARKER-ANTS-5088")))
        << "the live roadmap was cut at " << md.size() << " characters";
}
