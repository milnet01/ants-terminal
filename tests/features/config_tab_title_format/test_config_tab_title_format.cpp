// ANTS-1764 — tab_title_format is a fixed enum: validate getter + setter.
//
// INV-1 getter falls back to "title" for unknown stored strings.
// INV-2 setter rejects unknown values (prior valid value survives).
// INV-3 each of the four valid values round-trips.

#include "config.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>

namespace {

// Sandbox XDG_CONFIG_HOME so Config reads/writes a temp dir, not the
// user's real config. Restores the prior value on teardown.
struct XdgConfigHomeGuard {
    QByteArray prior;
    bool hadPrior = false;
    XdgConfigHomeGuard() {
        hadPrior = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        if (hadPrior) prior = qgetenv("XDG_CONFIG_HOME");
    }
    void set(const QByteArray &v) { qputenv("XDG_CONFIG_HOME", v); }
    ~XdgConfigHomeGuard() {
        if (hadPrior) qputenv("XDG_CONFIG_HOME", prior);
        else qunsetenv("XDG_CONFIG_HOME");
    }
};

QString configJsonPath() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
           + "/ants-terminal/config.json";
}

}  // namespace

TEST(ConfigTabTitleFormat, INV3_DefaultAndRoundTrip) {
    XdgConfigHomeGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    guard.set(tmp.path().toUtf8());

    {
        Config cfg;
        EXPECT_EQ(cfg.tabTitleFormat(), QStringLiteral("title"));  // default
    }

    for (const QString &v : {QStringLiteral("title"), QStringLiteral("cwd"),
                             QStringLiteral("process"),
                             QStringLiteral("cwd-process")}) {
        { Config a; a.setTabTitleFormat(v); }
        { Config b; EXPECT_EQ(b.tabTitleFormat(), v); }
    }
}

TEST(ConfigTabTitleFormat, INV2_SetterRejectsUnknown) {
    XdgConfigHomeGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    guard.set(tmp.path().toUtf8());

    { Config a; a.setTabTitleFormat(QStringLiteral("cwd")); }
    { Config b; b.setTabTitleFormat(QStringLiteral("garbage-not-a-format")); }
    {
        Config c;
        // The garbage write must be a no-op; the prior valid value survives.
        EXPECT_EQ(c.tabTitleFormat(), QStringLiteral("cwd"));
    }
}

TEST(ConfigTabTitleFormat, INV1_GetterFallbackOnHandEdit) {
    XdgConfigHomeGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    guard.set(tmp.path().toUtf8());

    // Materialise config.json via a save, then hand-edit a garbage value
    // directly (bypassing the setter — mimics a forward-version write).
    { Config a; a.setTabTitleFormat(QStringLiteral("process")); }

    const QString path = configJsonPath();
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    obj["tab_title_format"] = QStringLiteral("garbage-not-a-format");
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QJsonDocument(obj).toJson());
    f.close();

    Config b;
    EXPECT_EQ(b.tabTitleFormat(), QStringLiteral("title"));  // fallback
}
