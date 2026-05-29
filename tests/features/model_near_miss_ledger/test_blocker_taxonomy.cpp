// ANTS-1894 INV-9 — source-grep sentinel asserting the 7-token blocker
// taxonomy is present byte-for-byte in modelautoswitch.cpp. Catches an
// accidental rename / renumbering before it ships to disk.
#include <gtest/gtest.h>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

namespace {

// Locate src/modelautoswitch.cpp by walking up from the test binary's CWD.
// Tests run from the build directory; the source tree is one level up.
QString findSourceFile() {
    QDir d = QDir::current();
    for (int i = 0; i < 5; ++i) {
        const QString candidate = d.absoluteFilePath(
            QStringLiteral("src/modelautoswitch.cpp"));
        if (QFile::exists(candidate)) return candidate;
        if (!d.cdUp()) break;
    }
    // Fall back to the CMake-injected source dir if defined.
#ifdef ANTS_SRC_DIR
    return QStringLiteral(ANTS_SRC_DIR) + QStringLiteral("/src/modelautoswitch.cpp");
#else
    return QString();
#endif
}

}  // namespace

TEST(ModelNearMissLedger, Inv9TaxonomyTokensPresent) {
    const QString path = findSourceFile();
    ASSERT_FALSE(path.isEmpty())
        << "Could not locate src/modelautoswitch.cpp from "
        << QDir::current().absolutePath().toStdString();

    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray content = f.readAll();

    const QStringList tokens = {
        QStringLiteral("auto_switch_disabled"),
        QStringLiteral("focused_state_not_idle"),
        QStringLiteral("composer_not_empty"),
        QStringLiteral("target_equals_current"),
        QStringLiteral("ticks_target_stable_insufficient"),
        QStringLiteral("dwell_time_insufficient"),
        QStringLiteral("override_cooldown_active"),
        QStringLiteral("idle_end_of_session"),   // ANTS-1917 — 8th token
    };
    for (const QString &tok : tokens) {
        EXPECT_TRUE(content.contains(tok.toUtf8()))
            << "Missing v1 blocker token: " << tok.toStdString();
    }
}
