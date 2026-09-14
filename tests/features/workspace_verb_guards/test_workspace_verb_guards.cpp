// Workspace verb write safety and argument bounds — see spec.md. ANTS-5096.
// Source-scrape of src/remotecontrol_workspace.cpp.

#include <QFile>
#include <QFileInfo>
#include <QString>

#include <gtest/gtest.h>

namespace {

QString body(const QString &signature) {
    QFile f(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
            + QStringLiteral("/../../../src/remotecontrol_workspace.cpp"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    const QString src = QString::fromUtf8(f.readAll());
    const int start = src.indexOf(signature);
    if (start < 0) return QString();
    const int end = src.indexOf(QStringLiteral("\n}\n"), start);
    return end < 0 ? QString() : src.mid(start, end - start);
}

}  // namespace

// INV-1
TEST(WorkspaceVerbGuards, MutationProbeWritesAtomically) {
    const QString b = body(QStringLiteral("QJsonDocument RemoteControl::cmdMutationProbe("));
    ASSERT_FALSE(b.isEmpty());
    EXPECT_FALSE(b.contains(QStringLiteral("QIODevice::WriteOnly | QIODevice::Truncate")))
        << "a truncating write can leave the source file cut short";
    EXPECT_GE(b.count(QStringLiteral("QSaveFile w(check.resolved)")), 2);
}

// INV-2
TEST(WorkspaceVerbGuards, BuildTargetForValidatesCmakePath) {
    const QString b = body(QStringLiteral("QJsonDocument RemoteControl::cmdBuildTargetFor("));
    ASSERT_FALSE(b.isEmpty());
    EXPECT_TRUE(b.contains(QStringLiteral("QStringLiteral(\"cmake_path\"));")))
        << "cmake_path is opened without path validation";
}

// INV-3
TEST(WorkspaceVerbGuards, FileOutlineCapsPaths) {
    const QString b = body(QStringLiteral("QJsonDocument RemoteControl::cmdFileOutline("));
    ASSERT_FALSE(b.isEmpty());
    EXPECT_TRUE(b.contains(QStringLiteral("paths.size() > kMaxOutlinePaths")))
        << "file_outline's paths array has no count cap";
}
